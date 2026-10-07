/** Business controller for privacy overview and personal-data mutations. */
import { MemoryManager, MemoryPreferenceItem, UserMemorySnapshot } from '../utils/MemoryManager';
import { MemoRecord, MemoResult, MemoService } from '../utils/MemoService';
import { LocalReminderRecord, ReminderResult, ReminderService } from '../utils/ReminderService';
import { ConversationStore } from '../utils/ConversationStore';
import { PrivacyAuditReport, PrivacyAuditRunner } from '../utils/PrivacyAuditRunner';

export interface PersonalDataOverview {
  facts: string[];
  preferences: MemoryPreferenceItem[];
  topics: string[];
  conversationCount: number;
  memos: MemoRecord[];
  reminders: LocalReminderRecord[];
}

export type PersonalMemoryKind = 'fact' | 'preference' | 'topic';

export class PersonalDataController {
  private readonly filesDir: string;

  constructor(filesDir: string) {
    this.filesDir = filesDir;
  }

  async loadOverview(): Promise<PersonalDataOverview> {
    const snapshot: UserMemorySnapshot = await MemoryManager.getUserMemorySnapshot();
    const reminders: LocalReminderRecord[] = await ReminderService.getRecords();
    return {
      facts: snapshot.facts,
      preferences: snapshot.preferences,
      topics: snapshot.topics,
      conversationCount: snapshot.conversationCount,
      memos: MemoService.getRecords(),
      reminders
    };
  }

  runStorageAudit(): PrivacyAuditReport {
    return PrivacyAuditRunner.run(this.filesDir);
  }

  async deleteMemory(kind: PersonalMemoryKind, key: string): Promise<boolean> {
    if (kind === 'fact') return MemoryManager.deleteFact(key);
    if (kind === 'preference') return MemoryManager.deletePreference(key);
    return MemoryManager.deleteTopic(key);
  }

  clearLongTermMemory(): Promise<void> {
    return MemoryManager.clearUserMemory();
  }

  async clearAllPersonalData(): Promise<void> {
    const reminderResult: ReminderResult = await ReminderService.cancelAll();
    if (!reminderResult.success) throw new Error(reminderResult.message);
    const memoResult: MemoResult = await MemoService.clear();
    if (!memoResult.success) throw new Error(memoResult.message);
    await ConversationStore.clear();
    await MemoryManager.clearUserMemory();
  }
}
