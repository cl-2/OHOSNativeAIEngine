/**
 * LogSanitizer.ts - 日志脱敏工具
 *
 * 在日志输出前过滤敏感信息：
 * - 中国大陆手机号
 * - 中国大陆身份证号
 * - 邮箱地址
 * - 中文人名（"我叫XXX"、"我姓X"模式）
 * - 详细地址
 * - IP 地址
 */

// 敏感模式正则（编译一次复用）
const PATTERNS: RegExp[] = [
  // 手机号：1xx-xxxx-xxxx 或 1xxxxxxxxxx
  /1[3-9]\d[- ]?\d{4}[- ]?\d{4}/g,
  // 身份证：18 位数字（含 X）
  /\b[1-9]\d{5}(?:19|20)\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}[\dXx]\b/g,
  // 邮箱
  /\b[\w.-]+@[\w.-]+\.\w{2,4}\b/g,
  // IP 地址
  /\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/g,
  // "我姓X" / "我叫XXX"（中文人名）
  /我(?:姓|叫|是)([\u4e00-\u9fff]{1,6})[,，。！\s]/g,
  // "电话XXXX" / "手机XXXX"
  /(?:电话|手机|手机号|电话号码)[：:]?\s*\d{7,11}/g,
  // 地址模式（含"省/市/区/路/号/栋"等关键词的连续文本）
  /[\u4e00-\u9fff]{2,4}省[\u4e00-\u9fff]{2,10}市[\u4e00-\u9fff]{2,20}(?:区|路|街|道|巷|号)\d{0,10}/g,
];

// 脱敏替换函数
function maskMatch(match: string): string {
  const len: number = match.length;
  if (len <= 3) return '***';
  // 保留首尾各1位，中间用***代替
  return match[0] + '***' + match[len - 1];
}

/**
 * 脱敏日志文本
 */
export function sanitizeLog(text: string): string {
  if (!text) return text;
  let result: string = text;
  for (let i: number = 0; i < PATTERNS.length; i++) {
    result = result.replace(PATTERNS[i], maskMatch);
  }
  // 额外：过滤连续10位以上数字（可能是ID或其他敏感信息）
  result = result.replace(/\b\d{10,}\b/g, '**********');
  return result;
}

/**
 * 带脱敏的 console.info 快捷函数
 */
export function logInfo(tag: string, msg: string): void {
  console.info(tag + ': ' + sanitizeLog(msg));
}

/**
 * 带脱敏的 console.error 快捷函数
 */
export function logError(tag: string, msg: string): void {
  console.error(tag + ': ' + sanitizeLog(msg));
}
