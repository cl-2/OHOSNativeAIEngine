import { decideLocalSkill, LocalSkillDecision } from './LocalSkillRouter';
import { parseMemoCommand, MemoCommand } from './MemoParser';
import { parseReminderCommand, ReminderCommand } from './ReminderParser';

export type DeterministicSkillDomain = 'none' | 'reminder' | 'memo' | 'control';

/**
 * A stable envelope avoids page code invoking several parsers independently.
 * The first matching domain owns the utterance; execution remains with the UI.
 */
export interface DeterministicSkillRoute {
  domain: DeterministicSkillDomain;
  reminder: ReminderCommand;
  memo: MemoCommand;
  control: LocalSkillDecision;
}

function emptyReminder(): ReminderCommand {
  return { kind: 'none', triggerAtMs: 0, content: '', displayTime: '', error: '' };
}

function emptyMemo(): MemoCommand {
  return { kind: 'none', content: '', error: '' };
}

function emptyControl(): LocalSkillDecision {
  return { matched: false, skill: 'none', normalizedText: '' };
}

export function routeDeterministicSkill(input: string, nowMs: number = Date.now()): DeterministicSkillRoute {
  const reminder: ReminderCommand = parseReminderCommand(input, nowMs);
  if (reminder.kind !== 'none') {
    return { domain: 'reminder', reminder, memo: emptyMemo(), control: emptyControl() };
  }

  const memo: MemoCommand = parseMemoCommand(input);
  if (memo.kind !== 'none') {
    return { domain: 'memo', reminder: emptyReminder(), memo, control: emptyControl() };
  }

  const control: LocalSkillDecision = decideLocalSkill(input);
  if (control.matched) {
    return { domain: 'control', reminder: emptyReminder(), memo: emptyMemo(), control };
  }

  return { domain: 'none', reminder: emptyReminder(), memo: emptyMemo(), control };
}

export interface DeterministicSkillRouteVector {
  input: string;
  expected: DeterministicSkillDomain;
}

export function deterministicSkillRouteVectors(): DeterministicSkillRouteVector[] {
  return [
    { input: '三分钟后提醒我喝水', expected: 'reminder' },
    { input: '查看我的提醒', expected: 'reminder' },
    { input: '记一下三分钟后喝水', expected: 'memo' },
    { input: '看我的备忘录', expected: 'memo' },
    { input: '当前模型状态', expected: 'control' },
    { input: '语速快一点', expected: 'control' },
    { input: '提醒为什么很重要', expected: 'none' },
    { input: '帮我写一份备忘录模板', expected: 'none' }
  ];
}
