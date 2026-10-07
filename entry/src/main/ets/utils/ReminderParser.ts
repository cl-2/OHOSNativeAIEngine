/**
 * Pure, deterministic Chinese reminder parser.
 *
 * It intentionally accepts only explicit reminder/timer expressions. Ambiguous
 * sentences fall through to the LLM instead of silently scheduling something.
 */

export type ReminderCommandKind =
  'none' |
  'create' |
  'list' |
  'cancel_last' |
  'cancel_all_request' |
  'cancel_all_confirm';

export interface ReminderCommand {
  kind: ReminderCommandKind;
  triggerAtMs: number;
  content: string;
  displayTime: string;
  error: string;
}

function emptyCommand(): ReminderCommand {
  return { kind: 'none', triggerAtMs: 0, content: '', displayTime: '', error: '' };
}

function parseNumber(text: string): number {
  if (/^\d+$/.test(text)) return Number(text);
  const digitMap: Record<string, number> = {
    '零': 0, '〇': 0, '一': 1, '二': 2, '两': 2, '三': 3, '四': 4,
    '五': 5, '六': 6, '七': 7, '八': 8, '九': 9
  };
  if (text === '十') return 10;
  if (text.indexOf('百') >= 0) {
    const parts: string[] = text.split('百');
    const hundreds: number = parts[0] ? (digitMap[parts[0]] ?? 0) : 1;
    return hundreds * 100 + (parts[1] ? parseNumber(parts[1]) : 0);
  }
  if (text.indexOf('十') >= 0) {
    const parts: string[] = text.split('十');
    const tens: number = parts[0] ? (digitMap[parts[0]] ?? 0) : 1;
    const ones: number = parts.length > 1 && parts[1] ? (digitMap[parts[1]] ?? 0) : 0;
    return tens * 10 + ones;
  }
  let value: number = 0;
  for (let i: number = 0; i < text.length; i++) {
    if (digitMap[text[i]] === undefined) return -1;
    value = value * 10 + digitMap[text[i]];
  }
  return value;
}

function cleanContent(text: string, fallback: string): string {
  let value: string = text.trim();
  value = value.replace(/^(提醒我|叫我|通知我)/, '').replace(/[，。！？,.!?]+$/g, '').trim();
  return value || fallback;
}

function pad2(value: number): string {
  return value < 10 ? '0' + value : String(value);
}

function formatDateTime(date: Date): string {
  const now: Date = new Date();
  const tomorrow: Date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
  let prefix: string = (date.getMonth() + 1) + '月' + date.getDate() + '日';
  if (date.toDateString() === now.toDateString()) prefix = '今天';
  else if (date.toDateString() === tomorrow.toDateString()) prefix = '明天';
  return prefix + ' ' + pad2(date.getHours()) + ':' + pad2(date.getMinutes());
}

export function parseReminderCommand(input: string, nowMs: number = Date.now()): ReminderCommand {
  const raw: string = input.trim();
  const compact: string = raw.replace(/[，。！？、,.!?；;：:\s]/g, '');
  if (!compact) return emptyCommand();

  if (/^(查看|查询|列出|看看)?(我的|当前|所有)?(提醒|计时器)(列表|事项|任务)?$/.test(compact) ||
      /^(我有|还有)(什么|哪些|几个)(提醒|计时器)$/.test(compact)) {
    return { kind: 'list', triggerAtMs: 0, content: '', displayTime: '', error: '' };
  }
  if (/^(取消|删除)(最近|上一个|刚才)(的)?(提醒|计时器)$/.test(compact)) {
    return { kind: 'cancel_last', triggerAtMs: 0, content: '', displayTime: '', error: '' };
  }
  if (/^确认(取消|删除)(全部|所有)(提醒|计时器)$/.test(compact)) {
    return { kind: 'cancel_all_confirm', triggerAtMs: 0, content: '', displayTime: '', error: '' };
  }
  if (/^(取消|删除)(全部|所有)(提醒|计时器)$/.test(compact)) {
    return { kind: 'cancel_all_request', triggerAtMs: 0, content: '', displayTime: '', error: '' };
  }

  const numberExpr: string = '(\\d+|[零〇一二两三四五六七八九十百]+)';
  const relative: RegExp = new RegExp('^(?:(?:设置|创建|设|定)(?:一个)?)?' +
    numberExpr + '(秒钟?|分钟?|小时)(?:后)?(.*)$');
  const relativeMatch: RegExpMatchArray | null = compact.match(relative);
  if (relativeMatch) {
    const amount: number = parseNumber(relativeMatch[1]);
    const unit: string = relativeMatch[2];
    let seconds: number = amount;
    if (unit.indexOf('分钟') === 0) seconds *= 60;
    else if (unit === '小时') seconds *= 3600;
    if (amount <= 0 || seconds < 5) {
      return { kind: 'create', triggerAtMs: 0, content: '', displayTime: '', error: '提醒时间至少需要五秒。' };
    }
    if (seconds > 365 * 24 * 3600) {
      return { kind: 'create', triggerAtMs: 0, content: '', displayTime: '', error: '暂时只支持一年以内的提醒。' };
    }
    const isTimer: boolean = compact.indexOf('计时器') >= 0;
    const fallback: string = isTimer ? '计时结束' : '时间到了';
    let trailing: string = relativeMatch[3];
    trailing = trailing.replace(/^(设置|创建|设|定)(一个)?/, '').replace(/(提醒|计时器)$/, '');
    const content: string = cleanContent(trailing, fallback);
    const target: Date = new Date(nowMs + seconds * 1000);
    return { kind: 'create', triggerAtMs: target.getTime(), content, displayTime: formatDateTime(target), error: '' };
  }

  // Examples: 下午五点提醒我开会、明天上午9点半提醒我出门。
  const absolute: RegExp = new RegExp('^(今天|明天)?(凌晨|早上|上午|中午|下午|晚上)?' +
    numberExpr + '[点时](半|' + numberExpr + '分?)?(提醒我|叫我|通知我)(.+)$');
  const absoluteMatch: RegExpMatchArray | null = compact.match(absolute);
  if (absoluteMatch) {
    const dayWord: string = absoluteMatch[1] || '';
    const period: string = absoluteMatch[2] || '';
    let hour: number = parseNumber(absoluteMatch[3]);
    const minuteExpr: string = absoluteMatch[4] || '';
    let minute: number = minuteExpr === '半' ? 30 : (minuteExpr ? parseNumber(minuteExpr.replace('分', '')) : 0);
    if ((period === '下午' || period === '晚上') && hour < 12) hour += 12;
    if (period === '中午' && hour < 11) hour += 12;
    if (period === '凌晨' && hour === 12) hour = 0;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      return { kind: 'create', triggerAtMs: 0, content: '', displayTime: '', error: '这个提醒时间无法识别，请换一种说法。' };
    }
    const now: Date = new Date(nowMs);
    let dayOffset: number = dayWord === '明天' ? 1 : 0;
    let target: Date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + dayOffset, hour, minute, 0, 0);
    if (target.getTime() <= nowMs) {
      if (dayWord === '今天') {
        return { kind: 'create', triggerAtMs: 0, content: '', displayTime: '', error: '今天的这个时间已经过去了。' };
      }
      if (!dayWord) target = new Date(target.getTime() + 24 * 3600 * 1000);
    }
    const content: string = cleanContent(absoluteMatch[absoluteMatch.length - 1], '时间到了');
    return { kind: 'create', triggerAtMs: target.getTime(), content, displayTime: formatDateTime(target), error: '' };
  }

  return emptyCommand();
}

export interface ReminderParserVector {
  input: string;
  expected: ReminderCommandKind;
}

export function reminderParserRegressionVectors(): ReminderParserVector[] {
  return [
    { input: '三分钟后提醒我喝水', expected: 'create' },
    { input: '设置一个十分钟计时器', expected: 'create' },
    { input: '明天下午五点提醒我开会', expected: 'create' },
    { input: '查看我的提醒', expected: 'list' },
    { input: '取消最近的提醒', expected: 'cancel_last' },
    { input: '取消所有提醒', expected: 'cancel_all_request' },
    { input: '确认取消所有提醒', expected: 'cancel_all_confirm' },
    { input: '提醒为什么很重要', expected: 'none' },
    { input: '三分钟能做什么', expected: 'none' }
  ];
}
