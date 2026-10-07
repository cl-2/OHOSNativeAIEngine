/**
 * Deterministic, offline command matcher.
 *
 * Keep this module pure: no UI, audio, model or network side effects. The
 * caller owns execution. Exact/high-confidence phrases are intentional so a
 * normal question such as “介绍一下本地模型” is never stolen from the LLM.
 */

export type LocalSkillId =
  'none' |
  'stop_speaking' |
  'repeat_last' |
  'new_conversation' |
  'clear_conversation' |
  'route_local' |
  'route_smart' |
  'route_remote' |
  'status' |
  'speed_up' |
  'speed_down' |
  'speed_normal';

export interface LocalSkillDecision {
  matched: boolean;
  skill: LocalSkillId;
  normalizedText: string;
}

function normalizeCommand(input: string): string {
  let text: string = input.trim().toLowerCase();
  text = text.replace(/[，。！？、,.!?；;：:\s]/g, '');
  text = text.replace(/^(麻烦你|请你|麻烦|帮我|请)/, '');
  text = text.replace(/(一下吧|可以吗|好吗|一下|吧)$/, '');
  return text;
}

function isOneOf(text: string, values: string[]): boolean {
  for (let i: number = 0; i < values.length; i++) {
    if (text === values[i]) return true;
  }
  return false;
}

export function decideLocalSkill(input: string): LocalSkillDecision {
  const text: string = normalizeCommand(input);
  let skill: LocalSkillId = 'none';

  if (isOneOf(text, ['停止朗读', '停止播放', '别读了', '不要读了', '别说了', '停一下', '闭嘴'])) {
    skill = 'stop_speaking';
  } else if (isOneOf(text, ['重读上一条', '重新朗读上一条', '重复上一条回复', '再读一遍', '重新读一遍'])) {
    skill = 'repeat_last';
  } else if (isOneOf(text, ['新建对话', '开始新对话', '开启新对话', '创建新对话'])) {
    skill = 'new_conversation';
  } else if (isOneOf(text, ['清空对话', '清空当前对话', '清除当前对话', '删除当前对话记录'])) {
    skill = 'clear_conversation';
  } else if (isOneOf(text, ['切换到本地模式', '切换本地模式', '使用本地模型', '改用本地模型', '纯本地模式'])) {
    skill = 'route_local';
  } else if (isOneOf(text, ['切换到智能模式', '切换智能模式', '使用智能路由', '智能路由模式'])) {
    skill = 'route_smart';
  } else if (isOneOf(text, ['切换到远端模式', '切换远端模式', '使用远端模型', '改用远端模型', '云端模式'])) {
    skill = 'route_remote';
  } else if (isOneOf(text, [
    '模型状态', '查看模型状态', '当前模型状态', '当前是什么模型', '当前用的什么模型',
    '网络状态', '查看网络状态', '当前是什么模式', '当前模式', '隐私状态', '助手状态'
  ])) {
    skill = 'status';
  } else if (isOneOf(text, ['语速快一点', '说快一点', '朗读快一点', '调快语速', '加快语速'])) {
    skill = 'speed_up';
  } else if (isOneOf(text, ['语速慢一点', '说慢一点', '朗读慢一点', '调慢语速', '降低语速'])) {
    skill = 'speed_down';
  } else if (isOneOf(text, ['恢复正常语速', '正常语速', '默认语速', '语速恢复默认'])) {
    skill = 'speed_normal';
  }

  return { matched: skill !== 'none', skill, normalizedText: text };
}

export interface LocalSkillVector {
  input: string;
  expected: LocalSkillId;
}

export function localSkillRegressionVectors(): LocalSkillVector[] {
  return [
    { input: '请停止朗读', expected: 'stop_speaking' },
    { input: '再读一遍', expected: 'repeat_last' },
    { input: '开始新对话', expected: 'new_conversation' },
    { input: '清空当前对话', expected: 'clear_conversation' },
    { input: '切换到本地模式', expected: 'route_local' },
    { input: '使用智能路由', expected: 'route_smart' },
    { input: '切换到远端模式', expected: 'route_remote' },
    { input: '当前用的什么模型', expected: 'status' },
    { input: '语速快一点', expected: 'speed_up' },
    { input: '说慢一点', expected: 'speed_down' },
    { input: '恢复正常语速', expected: 'speed_normal' },
    // Negative vectors protect ordinary conversation from command hijacking.
    { input: '介绍一下本地模型', expected: 'none' },
    { input: '为什么你的语速比较慢', expected: 'none' },
    { input: '帮我写一段关于智能路由的介绍', expected: 'none' }
  ];
}
