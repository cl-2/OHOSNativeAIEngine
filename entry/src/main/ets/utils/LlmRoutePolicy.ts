export const ROUTING_LOCAL: number = 0;
export const ROUTING_SMART: number = 1;
export const ROUTING_REMOTE: number = 2;

export interface LlmRouteContext {
  mode: number;
  userText: string;
  localInstalled: boolean;
  online: boolean;
  remoteConfigured: boolean;
  remoteCoolingDown: boolean;
  remoteSourceLabel: string;
}

export interface LlmRouteDecision {
  target: string;
  reason: string;
  refusal: string;
}

function containsAny(text: string, keywords: string[]): boolean {
  for (let i: number = 0; i < keywords.length; i++) {
    if (text.includes(keywords[i])) return true;
  }
  return false;
}

/**
 * Pure routing policy shared by production code and deterministic regression tests.
 * It deliberately does not perform network/model I/O.
 */
export function decideLlmRoute(context: LlmRouteContext): LlmRouteDecision {
  if (context.mode === ROUTING_LOCAL) {
    return { target: 'local', reason: '📱 本机 GGUF · 用户指定纯本地', refusal: '' };
  }
  if (context.mode === ROUTING_REMOTE) {
    return { target: 'remote', reason: context.remoteSourceLabel + ' · 用户指定远端', refusal: '' };
  }

  const contextPos: number = context.userText.indexOf('\n\n[AI 之前说到:');
  const text: string = (contextPos >= 0 ? context.userText.substring(0, contextPos) : context.userText).trim();
  const privacyKeywords: string[] = [
    '我刚才', '我的姓名', '我的身份证', '我的手机号', '我的电话号码',
    '我的住址', '我的病历', '我的体检', '我的文件', '我的文档',
    '身份证', '手机号', '电话号码', '住址', '地址簿', '通讯录',
    '短信', '密码', '病历', '体检', '本地文档', '隐私'
  ];
  const deviceKeywords: string[] = [
    '打开', '关闭', '调高', '调低', '音量', '停止播放', '继续播放',
    '设置提醒', '设置闹钟', '开始录音', '停止录音'
  ];
  const freshKeywords: string[] = [
    '今天', '现在', '当前', '最新', '刚刚', '实时', '新闻', '天气',
    '气温', '股价', '价格', '汇率', '比分', '热搜', '现任'
  ];
  const factualKeywords: string[] = [
    '是谁', '简介', '资料', '出生', '哪里人', '多少岁', '代表作',
    '作品', '经历', '哪一年', '创始人', '作者', '歌手', '演员',
    '导演', '历史', '百科', '哪个公司', '哪家公司', '个人信息',
    '有什么歌', '唱过什么', '演过什么'
  ];

  if (containsAny(text, privacyKeywords)) {
    if (!context.localInstalled) {
      return {
        target: 'refuse', reason: '🔒 未发送 · 隐私内容',
        refusal: '这条内容可能包含隐私信息，但本地模型尚未安装。为避免上传，我没有发送到远端。'
      };
    }
    return { target: 'local', reason: '📱 本机 GGUF · 隐私内容不上传', refusal: '' };
  }

  if (containsAny(text, deviceKeywords) || containsAny(text, ['你自己', '你是谁', '你能做什么'])) {
    if (context.localInstalled) {
      return { target: 'local', reason: '📱 本机 GGUF · 本地任务', refusal: '' };
    }
  }

  if (containsAny(text, freshKeywords)) {
    return {
      target: 'refuse', reason: '⚠️ 未检索 · 时效事实',
      refusal: '这个问题需要实时检索才能可靠回答。当前应用只配置了对话模型，' +
        '尚未配置可验证的联网检索服务，所以我不会猜测。'
    };
  }

  if (containsAny(text, factualKeywords)) {
    if (context.online && context.remoteConfigured && !context.remoteCoolingDown) {
      return {
        target: 'remote', reason: context.remoteSourceLabel + ' · 事实查询（未联网检索）', refusal: ''
      };
    }
    return {
      target: 'refuse', reason: '⚠️ 未发送 · 事实无法核实',
      refusal: '这是事实型问题，但当前没有可用的远端模型，离线小模型无法可靠核实，因此我不编造答案。'
    };
  }

  if (context.localInstalled) {
    return { target: 'local', reason: '📱 本机 GGUF · 普通任务优先本地', refusal: '' };
  }
  if (context.online && context.remoteConfigured) {
    return { target: 'remote', reason: context.remoteSourceLabel + ' · 本地模型不可用', refusal: '' };
  }
  return {
    target: 'refuse', reason: '⚠️ 无可用模型',
    refusal: '本地模型尚未安装，远端模型也不可用。请先下载本地模型或配置自己的远端服务。'
  };
}
