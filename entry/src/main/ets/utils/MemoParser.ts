/** High-confidence deterministic parser for offline memo commands. */

export type MemoCommandKind =
  'none' |
  'create' |
  'list' |
  'delete_last' |
  'clear_request' |
  'clear_confirm';

export interface MemoCommand {
  kind: MemoCommandKind;
  content: string;
  error: string;
}

export function parseMemoCommand(input: string): MemoCommand {
  const raw: string = input.trim();
  const compact: string = raw.replace(/[，。！？、,.!?；;：:\s]/g, '');
  if (!compact) return { kind: 'none', content: '', error: '' };

  // Short ASR commands may collapse “查看” into “看”. The memo noun keeps
  // this tolerant form high-confidence, so ordinary uses of “看” are not intercepted.
  if (/^(查看|查询|打开|看看|看一下|看下|看)?(我的|全部|所有)?(备忘录|备忘|笔记)(列表|内容)?$/.test(compact) ||
      /^(我记了什么|我记过什么)$/.test(compact)) {
    return { kind: 'list', content: '', error: '' };
  }
  if (/^(删除|取消)(最近|上一条|刚才)(的)?(备忘|备忘录|笔记)$/.test(compact)) {
    return { kind: 'delete_last', content: '', error: '' };
  }
  if (/^确认(清空|删除)(全部|所有)(备忘|备忘录|笔记)$/.test(compact)) {
    return { kind: 'clear_confirm', content: '', error: '' };
  }
  if (/^(清空|删除)(全部|所有)(备忘|备忘录|笔记)$/.test(compact)) {
    return { kind: 'clear_request', content: '', error: '' };
  }

  const createMatch: RegExpMatchArray | null = raw.match(
    /^(?:请)?(?:帮我)?(?:记一下|记下|记录一下|添加备忘|新建备忘)[，,:：\s]*(.+)$/
  );
  if (createMatch) {
    const content: string = createMatch[1].replace(/[。！？.!?]+$/g, '').trim();
    if (!content) return { kind: 'create', content: '', error: '请告诉我要记录的内容。' };
    if (content.length > 500) return { kind: 'create', content: '', error: '单条备忘最多支持五百个字符。' };
    return { kind: 'create', content, error: '' };
  }
  return { kind: 'none', content: '', error: '' };
}

export interface MemoParserVector {
  input: string;
  expected: MemoCommandKind;
}

export function memoParserRegressionVectors(): MemoParserVector[] {
  return [
    { input: '记一下周五交报告', expected: 'create' },
    { input: '帮我记下门禁密码在抽屉里', expected: 'create' },
    { input: '查看我的备忘录', expected: 'list' },
    { input: '看我的备忘录', expected: 'list' },
    { input: '看看我的备忘录', expected: 'list' },
    { input: '看一下备忘', expected: 'list' },
    { input: '我记了什么', expected: 'list' },
    { input: '删除最近的备忘', expected: 'delete_last' },
    { input: '清空所有备忘', expected: 'clear_request' },
    { input: '确认清空所有备忘', expected: 'clear_confirm' },
    { input: '记忆是如何形成的', expected: 'none' },
    { input: '帮我写一份备忘录模板', expected: 'none' }
  ];
}
