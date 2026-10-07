/** Deterministic local-skill decisions without UI, Worker, or Native dependencies. */
import { LocalSkillDecision, LocalSkillId } from '../utils/LocalSkillRouter';
import { ReminderCommand } from '../utils/ReminderParser';
import { ReminderService } from '../utils/ReminderService';
import { MemoCommand } from '../utils/MemoParser';
import { MemoResult, MemoService } from '../utils/MemoService';

export type LocalControlEffect = 'none' | 'stop_tts' | 'new_conversation' |
  'clear_conversation' | 'route_local' | 'route_smart' | 'route_remote' | 'open_remote_settings';

export interface LocalControlRuntime {
  previousAssistantReply: string;
  runtimeStatus: string;
  speed: number;
  localModelInstalled: boolean;
  remoteConfigured: boolean;
}

export interface LocalControlResult {
  response: string;
  speechText: string;
  includeUser: boolean;
  effect: LocalControlEffect;
  nextSpeed: number;
}

export interface LocalOperationResult {
  success: boolean;
  message: string;
}

export class LocalSkillController {
  private pendingCancelAllRemindersUntil: number = 0;
  private pendingClearMemosUntil: number = 0;

  resolveControl(decision: LocalSkillDecision, runtime: LocalControlRuntime): LocalControlResult {
    const skill: LocalSkillId = decision.skill;
    if (skill === 'stop_speaking') {
      return this.result('已停止朗读。', '', true, 'stop_tts', runtime.speed);
    }
    if (skill === 'repeat_last') {
      if (runtime.previousAssistantReply) {
        return this.result('正在重新朗读上一条回复。', runtime.previousAssistantReply,
          true, 'none', runtime.speed);
      }
      return this.result('当前还没有可以重新朗读的回复。', '当前还没有可以重新朗读的回复。',
        true, 'none', runtime.speed);
    }
    if (skill === 'new_conversation') {
      return this.result('新对话已经准备好了。', '新对话已经准备好了。',
        false, 'new_conversation', runtime.speed);
    }
    if (skill === 'clear_conversation') {
      return this.result('当前对话已经清空。', '当前对话已经清空。',
        false, 'clear_conversation', runtime.speed);
    }
    if (skill === 'route_local') {
      const message: string = runtime.localModelInstalled ? '已切换到纯本地模式。' :
        '已切换到纯本地模式，请先下载本地模型。';
      return this.result(message, message, true, 'route_local', runtime.speed);
    }
    if (skill === 'route_smart') {
      const message: string = runtime.remoteConfigured ? '已切换到智能路由。' :
        '已切换到智能路由。远端尚未配置，事实问题不会上传或猜测。';
      return this.result(message, message, true, 'route_smart', runtime.speed);
    }
    if (skill === 'route_remote') {
      const message: string = runtime.remoteConfigured ? '已切换到指定远端模式。' :
        '远端模型尚未配置，已打开模型设置。';
      return this.result(message, message, true,
        runtime.remoteConfigured ? 'route_remote' : 'open_remote_settings', runtime.speed);
    }
    if (skill === 'status') {
      return this.result(runtime.runtimeStatus, runtime.runtimeStatus, true, 'none', runtime.speed);
    }
    if (skill === 'speed_up') {
      const speed: number = Math.min(1.3, Math.round((runtime.speed + 0.1) * 10) / 10);
      const message: string = speed >= 1.3 ? '朗读速度已调到最快。' :
        '朗读速度已调快到 ' + speed.toFixed(1) + ' 倍。';
      return this.result(message, message, true, 'none', speed);
    }
    if (skill === 'speed_down') {
      const speed: number = Math.max(0.7, Math.round((runtime.speed - 0.1) * 10) / 10);
      const message: string = speed <= 0.7 ? '朗读速度已调到最慢。' :
        '朗读速度已调慢到 ' + speed.toFixed(1) + ' 倍。';
      return this.result(message, message, true, 'none', speed);
    }
    return this.result('朗读速度已恢复正常。', '朗读速度已恢复正常。',
      true, 'none', 1.0);
  }

  executeReminder(command: ReminderCommand): Promise<LocalOperationResult> {
    if (command.kind === 'create') {
      if (command.error) return Promise.resolve({ success: false, message: command.error });
      return ReminderService.create(command);
    }
    if (command.kind === 'list') return ReminderService.list();
    if (command.kind === 'cancel_last') return ReminderService.cancelLast();
    if (command.kind === 'cancel_all_request') {
      this.pendingCancelAllRemindersUntil = Date.now() + 15000;
      return Promise.resolve({
        success: true,
        message: '这会取消全部提醒。请在十五秒内说“确认取消所有提醒”。'
      });
    }
    if (command.kind === 'cancel_all_confirm') {
      if (Date.now() <= this.pendingCancelAllRemindersUntil) {
        this.pendingCancelAllRemindersUntil = 0;
        return ReminderService.cancelAll();
      }
      return Promise.resolve({ success: false, message: '确认已经超时，请先说“取消所有提醒”。' });
    }
    return Promise.resolve({ success: false, message: '没有识别到提醒指令。' });
  }

  async executeMemo(command: MemoCommand): Promise<LocalOperationResult> {
    let result: MemoResult;
    if (command.kind === 'create') {
      result = command.error ? { success: false, message: command.error } :
        await MemoService.add(command.content);
    } else if (command.kind === 'list') {
      result = await MemoService.describeList();
    } else if (command.kind === 'delete_last') {
      result = await MemoService.deleteLast();
    } else if (command.kind === 'clear_request') {
      this.pendingClearMemosUntil = Date.now() + 15000;
      result = { success: true, message: '这会清空全部备忘。请在十五秒内说“确认清空所有备忘”。' };
    } else if (command.kind === 'clear_confirm') {
      if (Date.now() <= this.pendingClearMemosUntil) {
        this.pendingClearMemosUntil = 0;
        result = await MemoService.clear();
      } else {
        result = { success: false, message: '确认已经超时，请先说“清空所有备忘”。' };
      }
    } else {
      result = { success: false, message: '没有识别到备忘指令。' };
    }
    return result;
  }

  private result(response: string, speechText: string, includeUser: boolean,
    effect: LocalControlEffect, nextSpeed: number): LocalControlResult {
    return { response, speechText, includeUser, effect, nextSpeed };
  }
}
