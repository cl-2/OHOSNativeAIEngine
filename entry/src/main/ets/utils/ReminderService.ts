import { reminderAgentManager } from '@kit.BackgroundTasksKit';
import { fileIo } from '@kit.CoreFileKit';
import { notificationManager } from '@kit.NotificationKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { ReminderCommand } from './ReminderParser';
import { EncryptedStore } from './EncryptedStore';

export interface LocalReminderRecord {
  reminderId: number;
  content: string;
  triggerAtMs: number;
  createdAtMs: number;
  scheduler?: string;
}

export interface ReminderResult {
  success: boolean;
  message: string;
}

export type ReminderTriggerCallback = (content: string) => void;

interface ReminderStoreData {
  version: number;
  reminders: LocalReminderRecord[];
}

const BUNDLE_NAME: string = 'com.example.ohosnativeaiengine';
const ABILITY_NAME: string = 'EntryAbility';

export class ReminderService {
  private static storePath: string = '';
  private static records: LocalReminderRecord[] = [];
  private static abilityContext?: common.UIAbilityContext;
  private static orphanedSystemCount: number = 0;
  private static localTimers: Map<number, number> = new Map<number, number>();
  private static triggerCallback?: ReminderTriggerCallback;
  private static readyPromise: Promise<void> = Promise.resolve();
  private static writeQueue: Promise<void> = Promise.resolve();

  static init(filesDir: string, context: common.UIAbilityContext,
    callback: ReminderTriggerCallback): Promise<void> {
    this.storePath = filesDir + '/reminders/reminders.json';
    this.abilityContext = context;
    this.triggerCallback = callback;
    this.readyPromise = this.initializeRecords();
    notificationManager.addSlot(notificationManager.SlotType.SOCIAL_COMMUNICATION).catch((e: Object): void => {
      console.warn('OHOS_REMINDER: add notification slot failed=' + String(e));
    });
    return this.readyPromise;
  }

  static async create(command: ReminderCommand): Promise<ReminderResult> {
    await this.readyPromise;
    if (!this.storePath) return { success: false, message: '提醒服务尚未初始化。' };
    if (command.error) return { success: false, message: command.error };
    const notificationReady: ReminderResult = await this.ensureNotificationEnabled();
    if (!notificationReady.success) return notificationReady;
    const seconds: number = Math.max(5, Math.ceil((command.triggerAtMs - Date.now()) / 1000));
    if (command.triggerAtMs <= Date.now()) return { success: false, message: '提醒时间已经过去了。' };
    const notificationId: number = Math.floor(Date.now() % 2000000000);
    const request: reminderAgentManager.ReminderRequestTimer = {
      reminderType: reminderAgentManager.ReminderType.REMINDER_TYPE_TIMER,
      triggerTimeInSeconds: seconds,
      title: '本地语音助手提醒',
      content: command.content,
      expiredContent: command.content,
      snoozeContent: '稍后提醒：' + command.content,
      notificationId,
      ringDuration: 10,
      snoozeTimes: 1,
      timeInterval: 300,
      tapDismissed: true,
      slotType: notificationManager.SlotType.SOCIAL_COMMUNICATION,
      actionButton: [
        { title: '关闭', type: reminderAgentManager.ActionButtonType.ACTION_BUTTON_TYPE_CLOSE },
        { title: '稍后提醒', type: reminderAgentManager.ActionButtonType.ACTION_BUTTON_TYPE_SNOOZE }
      ],
      wantAgent: { pkgName: BUNDLE_NAME, abilityName: ABILITY_NAME }
    };
    try {
      const reminderId: number = await reminderAgentManager.publishReminder(request);
      this.records.push({
        reminderId,
        content: command.content,
        triggerAtMs: command.triggerAtMs,
        createdAtMs: Date.now(),
        scheduler: 'system'
      });
      this.persist();
      return { success: true, message: '好的，已设置在' + command.displayTime + '提醒你' + command.content + '。' };
    } catch (e) {
      const error: BusinessError = e as BusinessError;
      if (error.code === 1700002) {
        const systemCount: number = await this.querySystemReminderCount();
        if (systemCount === 0) {
          // Some commercial devices assign a zero Reminder Agent quota to an
          // ordinary/debug-signed application even though the manifest
          // permission itself is granted. Keep the user-facing skill usable
          // while this process is alive and restore it on the next launch.
          console.warn('OHOS_REMINDER: system quota is zero, using local scheduler');
          return this.createLocalFallback(command);
        }
        const countText: string = systemCount >= 0 ? '（当前系统记录为' + systemCount + '个）' : '';
        console.warn('OHOS_REMINDER: system limit reached count=' + systemCount);
        return {
          success: false,
          message: '系统提醒已达到上限' + countText + '，可能是旧版本遗留。请说“取消所有提醒”，再说“确认取消所有提醒”后重试。'
        };
      }
      return { success: false, message: this.describeError(e, '设置提醒失败') };
    }
  }

  static async list(): Promise<ReminderResult> {
    await this.readyPromise;
    await this.reconcileWithSystem();
    this.pruneExpired();
    if (this.records.length === 0) {
      if (this.orphanedSystemCount > 0) {
        return {
          success: true,
          message: '系统中有' + this.orphanedSystemCount +
            '个旧版本遗留提醒，但本地已没有对应内容。可以说“取消所有提醒”进行清理。'
        };
      }
      return { success: true, message: '当前没有待执行的提醒。' };
    }
    const sorted: LocalReminderRecord[] = this.records.slice().sort(
      (a: LocalReminderRecord, b: LocalReminderRecord): number => a.triggerAtMs - b.triggerAtMs
    );
    const parts: string[] = [];
    const maxCount: number = Math.min(sorted.length, 5);
    for (let i: number = 0; i < maxCount; i++) {
      parts.push((i + 1) + '，' + this.formatDateTime(sorted[i].triggerAtMs) + '，' + sorted[i].content);
    }
    let suffix: string = '。';
    if (sorted.length > maxCount) suffix = '。另外还有' + (sorted.length - maxCount) + '个提醒。';
    if (this.orphanedSystemCount > 0) {
      suffix += '系统中另有' + this.orphanedSystemCount + '个旧版本遗留提醒。';
    }
    return { success: true, message: '你有' + sorted.length + '个待执行提醒：' + parts.join('；') + suffix };
  }

  static async getRecords(): Promise<LocalReminderRecord[]> {
    await this.readyPromise;
    await this.reconcileWithSystem();
    this.pruneExpired();
    return this.records.slice().sort(
      (a: LocalReminderRecord, b: LocalReminderRecord): number => a.triggerAtMs - b.triggerAtMs
    );
  }

  static async cancelById(reminderId: number): Promise<ReminderResult> {
    await this.readyPromise;
    let target: LocalReminderRecord | undefined;
    for (let i: number = 0; i < this.records.length; i++) {
      if (this.records[i].reminderId === reminderId) {
        target = this.records[i];
        break;
      }
    }
    if (!target) return { success: false, message: '这个提醒已经不存在了。' };
    if (target.scheduler === 'local' || target.reminderId < 0) {
      this.clearLocalTimer(target.reminderId);
      this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== reminderId);
      this.persist();
      return { success: true, message: '已取消“' + target.content + '”提醒。' };
    }
    try {
      await reminderAgentManager.cancelReminder(target.reminderId);
      this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== reminderId);
      this.persist();
      return { success: true, message: '已取消“' + target.content + '”提醒。' };
    } catch (e) {
      const error: BusinessError = e as BusinessError;
      if (error.code === 1700003) {
        this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== reminderId);
        this.persist();
        return { success: true, message: '这个提醒已经执行或被取消了。' };
      }
      return { success: false, message: this.describeError(error, '取消提醒失败') };
    }
  }

  static async cancelLast(): Promise<ReminderResult> {
    await this.readyPromise;
    await this.reconcileWithSystem();
    this.pruneExpired();
    if (this.records.length === 0) return { success: true, message: '当前没有可以取消的提醒。' };
    let target: LocalReminderRecord = this.records[0];
    for (let i: number = 1; i < this.records.length; i++) {
      if (this.records[i].createdAtMs > target.createdAtMs) target = this.records[i];
    }
    if (target.scheduler === 'local' || target.reminderId < 0) {
      this.clearLocalTimer(target.reminderId);
      this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== target.reminderId);
      this.persist();
      return { success: true, message: '已取消' + this.formatDateTime(target.triggerAtMs) + '的“' + target.content + '”提醒。' };
    }
    try {
      await reminderAgentManager.cancelReminder(target.reminderId);
      this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== target.reminderId);
      this.persist();
      return { success: true, message: '已取消' + this.formatDateTime(target.triggerAtMs) + '的“' + target.content + '”提醒。' };
    } catch (e) {
      const message: string = String(e);
      // The reminder may have fired between reconciliation and cancellation.
      if (message.indexOf('1700003') >= 0 || message.toLowerCase().indexOf('does not exist') >= 0) {
        this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== target.reminderId);
        this.persist();
        return { success: true, message: '这个提醒已经执行或被取消了。' };
      }
      return { success: false, message: this.describeError(e, '取消提醒失败') };
    }
  }

  static async cancelAll(): Promise<ReminderResult> {
    await this.readyPromise;
    this.clearAllLocalTimers();
    this.records = this.records.filter((item: LocalReminderRecord): boolean =>
      item.scheduler !== 'local' && item.reminderId >= 0);
    this.persist();
    try {
      await reminderAgentManager.cancelAllReminders();
      this.records = [];
      this.orphanedSystemCount = 0;
      this.persist();
      return { success: true, message: '已取消全部提醒。' };
    } catch (e) {
      return { success: false, message: this.describeError(e, '取消全部提醒失败') };
    }
  }

  private static async reconcileWithSystem(): Promise<void> {
    try {
      const valid: reminderAgentManager.ReminderInfo[] = await reminderAgentManager.getAllValidReminders();
      const ids: number[] = [];
      for (let i: number = 0; i < valid.length; i++) ids.push(valid[i].reminderId);
      let knownSystemCount: number = 0;
      for (let i: number = 0; i < this.records.length; i++) {
        if (ids.indexOf(this.records[i].reminderId) >= 0) knownSystemCount++;
      }
      this.orphanedSystemCount = Math.max(0, valid.length - knownSystemCount);
      const before: number = this.records.length;
      this.records = this.records.filter((item: LocalReminderRecord): boolean =>
        item.scheduler === 'local' || item.reminderId < 0 || ids.indexOf(item.reminderId) >= 0);
      if (before !== this.records.length) this.persist();
    } catch (e) {
      // Local metadata still provides a useful list if a vendor build does not
      // expose the API despite declaring the system capability.
      console.warn('OHOS_REMINDER: reconcile failed=' + String(e));
    }
  }

  private static pruneExpired(): void {
    const cutoff: number = Date.now() - 60 * 1000;
    const before: number = this.records.length;
    this.records = this.records.filter((item: LocalReminderRecord): boolean => item.triggerAtMs >= cutoff);
    if (before !== this.records.length && this.storePath) this.persist();
  }

  private static async initializeRecords(): Promise<void> {
    const existed: boolean = this.pathExists(this.storePath);
    const data: ReminderStoreData | null = await EncryptedStore.read<ReminderStoreData>(this.storePath);
    if (existed && data === null) {
      // Do not replace an unreadable or authentication-failed file with an
      // empty list. Keeping it intact makes diagnosis and recovery possible.
      throw new Error('existing reminder store could not be authenticated or parsed');
    }
    this.records = data && data.reminders ? this.normalizeRecords(data.reminders) : [];
    this.pruneExpired();
    this.armLocalRecords();
    console.info('OHOS_REMINDER: encrypted store ready count=' + this.records.length);
  }

  private static persist(): Promise<void> {
    const snapshot: LocalReminderRecord[] = [];
    for (let i: number = 0; i < this.records.length; i++) {
      const item: LocalReminderRecord = this.records[i];
      snapshot.push({
        reminderId: item.reminderId,
        content: item.content,
        triggerAtMs: item.triggerAtMs,
        createdAtMs: item.createdAtMs,
        scheduler: item.scheduler
      });
    }
    const data: ReminderStoreData = { version: 2, reminders: snapshot };
    const operation: Promise<void> = this.writeQueue.then((): Promise<void> =>
      EncryptedStore.write(this.storePath, data)
    );
    this.writeQueue = operation.catch((e: Error): void => {
      console.warn('OHOS_REMINDER: encrypted persist failed=' + String(e));
    });
    return operation;
  }

  private static normalizeRecords(values: LocalReminderRecord[]): LocalReminderRecord[] {
    const normalized: LocalReminderRecord[] = [];
    for (let i: number = 0; i < values.length; i++) {
      const item: LocalReminderRecord = values[i];
      if (!item || typeof item.reminderId !== 'number' || typeof item.content !== 'string' ||
          typeof item.triggerAtMs !== 'number' || typeof item.createdAtMs !== 'number') continue;
      normalized.push({
        reminderId: item.reminderId,
        content: item.content,
        triggerAtMs: item.triggerAtMs,
        createdAtMs: item.createdAtMs,
        scheduler: item.scheduler === 'system' ? 'system' : 'local'
      });
    }
    return normalized;
  }

  private static describeError(error: Object, prefix: string): string {
    const businessError: BusinessError = error as BusinessError;
    const code: number = businessError.code;
    const detail: string = businessError.message || String(error);
    const text: string = 'code=' + code + ', message=' + detail;
    if (code === 1700001 || code === 1600004 || detail.toLowerCase().indexOf('not enabled') >= 0 ||
        detail.toLowerCase().indexOf('disabled') >= 0) {
      return '系统通知未开启，请在系统设置中允许本应用通知后再试。';
    }
    if (code === 1700002) return '待执行提醒数量已达到系统上限，请先取消一些提醒。';
    if (code === 201 || detail.toLowerCase().indexOf('permission') >= 0) {
      return '系统拒绝了提醒权限，请检查应用权限或重新安装应用。';
    }
    console.error('OHOS_REMINDER: ' + prefix + '=' + text);
    return prefix + '，请稍后再试。';
  }

  private static async ensureNotificationEnabled(): Promise<ReminderResult> {
    try {
      if (await notificationManager.isNotificationEnabled()) {
        return { success: true, message: '' };
      }
      if (!this.abilityContext) {
        return { success: false, message: '无法获取通知授权上下文，请重新打开应用后再试。' };
      }
      await notificationManager.requestEnableNotification(this.abilityContext);
      if (await notificationManager.isNotificationEnabled()) {
        return { success: true, message: '' };
      }
      return { success: false, message: '系统通知未开启，无法创建可靠提醒。' };
    } catch (e) {
      const error: BusinessError = e as BusinessError;
      console.warn('OHOS_REMINDER: notification authorization code=' + error.code +
        ', message=' + error.message);
      return { success: false, message: this.describeError(error, '通知授权失败') };
    }
  }

  private static async querySystemReminderCount(): Promise<number> {
    try {
      const valid: reminderAgentManager.ReminderInfo[] = await reminderAgentManager.getAllValidReminders();
      return valid.length;
    } catch (e) {
      console.warn('OHOS_REMINDER: query system count failed=' + String(e));
      return -1;
    }
  }

  private static createLocalFallback(command: ReminderCommand): ReminderResult {
    let reminderId: number = -Math.max(1, Math.floor(Date.now() % 2000000000));
    while (this.records.some((item: LocalReminderRecord): boolean => item.reminderId === reminderId)) reminderId--;
    const record: LocalReminderRecord = {
      reminderId,
      content: command.content,
      triggerAtMs: command.triggerAtMs,
      createdAtMs: Date.now(),
      scheduler: 'local'
    };
    this.records.push(record);
    this.persist();
    this.armLocalRecord(record);
    return { success: true, message: '好的，已设置在' + command.displayTime + '提醒你' + command.content + '。' };
  }

  private static armLocalRecords(): void {
    for (let i: number = 0; i < this.records.length; i++) {
      const record: LocalReminderRecord = this.records[i];
      if (record.scheduler === 'local' || record.reminderId < 0) this.armLocalRecord(record);
    }
  }

  private static armLocalRecord(record: LocalReminderRecord): void {
    this.clearLocalTimer(record.reminderId);
    const remaining: number = record.triggerAtMs - Date.now();
    if (remaining <= 0) {
      this.fireLocalReminder(record);
      return;
    }
    // Re-arm long timers in bounded chunks to avoid the platform timer's
    // signed 32-bit delay limit.
    const delay: number = Math.min(remaining, 12 * 60 * 60 * 1000);
    const timerId: number = setTimeout((): void => {
      this.localTimers.delete(record.reminderId);
      if (record.triggerAtMs > Date.now()) this.armLocalRecord(record);
      else this.fireLocalReminder(record);
    }, delay);
    this.localTimers.set(record.reminderId, timerId);
  }

  private static fireLocalReminder(record: LocalReminderRecord): void {
    const request: notificationManager.NotificationRequest = {
      id: Math.abs(record.reminderId),
      notificationSlotType: notificationManager.SlotType.SOCIAL_COMMUNICATION,
      content: {
        notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
        normal: { title: '本地语音助手提醒', text: record.content }
      }
    };
    notificationManager.publish(request).then((): void => {
      console.info('OHOS_REMINDER: local reminder fired id=' + record.reminderId);
    }).catch((e: Object): void => {
      console.error('OHOS_REMINDER: local notification failed=' + this.describeError(e, '发布通知失败'));
    });
    if (this.triggerCallback) {
      try {
        this.triggerCallback(record.content);
      } catch (e) {
        console.error('OHOS_REMINDER: trigger callback failed=' + String(e));
      }
    }
    this.records = this.records.filter((item: LocalReminderRecord): boolean => item.reminderId !== record.reminderId);
    this.persist();
  }

  private static clearLocalTimer(reminderId: number): void {
    const timerId: number | undefined = this.localTimers.get(reminderId);
    if (timerId !== undefined) clearTimeout(timerId);
    this.localTimers.delete(reminderId);
  }

  private static clearAllLocalTimers(): void {
    this.localTimers.forEach((timerId: number): void => clearTimeout(timerId));
    this.localTimers.clear();
  }

  private static pathExists(path: string): boolean {
    try {
      return fileIo.accessSync(path);
    } catch (e) {
      const message: string = String(e).toLowerCase();
      if (message.indexOf('no such file') >= 0 || message.indexOf('13900002') >= 0) return false;
      throw e;
    }
  }

  private static formatDateTime(timestamp: number): string {
    const date: Date = new Date(timestamp);
    const now: Date = new Date();
    const tomorrow: Date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
    let prefix: string = (date.getMonth() + 1) + '月' + date.getDate() + '日';
    if (date.toDateString() === now.toDateString()) prefix = '今天';
    else if (date.toDateString() === tomorrow.toDateString()) prefix = '明天';
    const hour: string = date.getHours() < 10 ? '0' + date.getHours() : String(date.getHours());
    const minute: string = date.getMinutes() < 10 ? '0' + date.getMinutes() : String(date.getMinutes());
    return prefix + hour + ':' + minute;
  }
}
