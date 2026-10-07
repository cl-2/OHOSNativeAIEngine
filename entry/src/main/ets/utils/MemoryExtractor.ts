export interface MemoryFieldUpdate {
  key: string;
  value: string;
}

export interface StructuredMemoryExtraction {
  fields: MemoryFieldUpdate[];
  liked: string[];
  disliked: string[];
  facts: string[];
  topics: string[];
}

const QUESTION_WORDS: string[] = ['什么', '哪个', '哪些', '哪种', '哪里', '怎么', '是否', '是不是', '记得吗'];

function emptyExtraction(): StructuredMemoryExtraction {
  return { fields: [], liked: [], disliked: [], facts: [], topics: [] };
}

function containsQuestion(value: string): boolean {
  for (let i: number = 0; i < QUESTION_WORDS.length; i++) {
    if (value.indexOf(QUESTION_WORDS[i]) >= 0) return true;
  }
  return /[？?]/.test(value) || /吗$|么$|呢$/.test(value);
}

function cleanValue(value: string): string {
  return value.trim()
    .replace(/^(就是|是|比较|很)/, '')
    .replace(/(但是|不过|然后|接着|但|了|啦|呀|啊|的)$|^(一个|一种)/, '')
    .trim();
}

function addUnique(values: string[], value: string): void {
  const normalized: string = cleanValue(value);
  if (!normalized || normalized.length > 24 || containsQuestion(normalized)) return;
  if (values.indexOf(normalized) < 0) values.push(normalized);
}

function splitValues(value: string): string[] {
  const raw: string[] = value.split(/(?:、|和|以及|还有|与|,|，)/);
  const result: string[] = [];
  for (let i: number = 0; i < raw.length; i++) addUnique(result, raw[i]);
  return result;
}

function setField(result: StructuredMemoryExtraction, key: string, value: string): void {
  const normalized: string = cleanValue(value);
  if (!normalized || containsQuestion(normalized)) return;
  for (let i: number = 0; i < result.fields.length; i++) {
    if (result.fields[i].key === key) {
      result.fields[i].value = normalized;
      return;
    }
  }
  result.fields.push({ key, value: normalized });
}

function addMatches(target: string[], value: string): void {
  const values: string[] = splitValues(value);
  for (let i: number = 0; i < values.length; i++) addUnique(target, values[i]);
}

/**
 * Deterministic, fully local extraction for high-confidence user statements.
 * It intentionally favors precision over recall; uncertain text remains only
 * in conversation history and is not promoted to long-term memory.
 */
export function extractStructuredMemory(userText: string): StructuredMemoryExtraction {
  const result: StructuredMemoryExtraction = emptyExtraction();
  const text: string = userText.replace(/\s+/g, ' ').trim()
    .replace(/(但是|不过|但)不再喜欢/g, '$1我不再喜欢')
    .replace(/(但是|不过|但)不喜欢/g, '$1我不喜欢');
  if (!text) return result;

  let match: RegExpExecArray | null =
    /我(?:叫|姓名是)([^，。！？!?；;我]{1,12}?)(?:我(?:今年|现在|来自|住在|是)|[,，。！？!?；;]|$)/.exec(text);
  if (match) setField(result, '称呼', match[1]);

  match = /我(?:现在)?(?:住在|来自)([^，。！？!?；;我]{1,16}?)(?:工作|生活|上学|居住|我(?:是|喜欢|今年)|[,，。！？!?；;]|$)/.exec(text);
  if (match) setField(result, '所在地', match[1]);

  match = /我是(?:一名|一个)?(工程师|设计师|学生|老师|医生|程序员|产品经理|运营|自由职业者)/.exec(text);
  if (match) setField(result, '职业', match[1]);

  match = /我(?:今年)?(\d{1,3}|[一二三四五六七八九十百]{1,3})岁/.exec(text);
  if (match) setField(result, '年龄', match[1] + '岁');

  // Parse negative preference first. A later positive statement in the same
  // utterance can still win when MemoryManager applies the positive set last.
  match = /我(?:现在|目前)?(?:已经)?(?:不再|不)(?:喜欢|爱)([^，。！？!?；;我]{1,30})/.exec(text);
  if (match) addMatches(result.disliked, match[1]);
  match = /我(?:现在|目前)?(?:很|非常)?(?:讨厌|不喜欢)([^，。！？!?；;我]{1,30})/.exec(text);
  if (match) addMatches(result.disliked, match[1]);

  match = /我(?:现在|目前)?(?:很|非常|比较|最)?(?:喜欢|热爱)([^，。！？!?；;我]{1,30})/.exec(text);
  if (match) addMatches(result.liked, match[1]);
  match = /我的爱好是([^，。！？!?；;我]{1,30})/.exec(text);
  if (match) addMatches(result.liked, match[1]);

  match = /我有(.{1,10})(只|条|个)(猫|狗|宠物)/.exec(text);
  if (match) addUnique(result.facts, '用户有' + match[1] + match[2] + match[3]);

  const topicKeywords: string[][] = [
    ['天气', '下雨', '下雪', '温度', '台风'],
    ['编程', '代码', '程序', '开发', '软件', '算法'],
    ['健康', '跑步', '运动', '健身', '瑜伽', '体检'],
    ['美食', '做饭', '菜谱', '餐厅', '吃饭'],
    ['旅行', '旅游', '出差', '机票', '酒店'],
    ['音乐', '电影', '读书', '小说', '游戏']
  ];
  for (let i: number = 0; i < topicKeywords.length; i++) {
    for (let j: number = 0; j < topicKeywords[i].length; j++) {
      if (text.indexOf(topicKeywords[i][j]) >= 0) {
        addUnique(result.topics, topicKeywords[i][0]);
        break;
      }
    }
  }
  return result;
}
