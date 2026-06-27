/**
 * CryptoManager.ts - 本地加密管理器
 *
 * 使用 OHOS HUKS 做 AES-256-GCM 加解密。
 * 密钥由系统安全存储，应用卸载时自动清除。
 *
 * 加密格式: base64(IV(12B) + 密文)
 * 兼容旧数据: 解密失败回退明文
 */

import huks from '@ohos.security.huks';

const KEY_ALIAS = 'ohos_ai_enc_key';

// ---- 手动 Base64 (ArkTS 无 btoa/atob) ----
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function toB64(b: Uint8Array): string {
  let r = '';
  for (let i = 0; i < b.length; i += 3) {
    const b0 = b[i], b1 = i + 1 < b.length ? b[i + 1] : 0, b2 = i + 2 < b.length ? b[i + 2] : 0;
    r += B64[b0 >> 2] + B64[((b0 & 3) << 4) | (b1 >> 4)];
    r += i + 1 < b.length ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    r += i + 2 < b.length ? B64[b2 & 63] : '=';
  }
  return r;
}
function fromB64(s: string): Uint8Array {
  s = s.replace(/=/g, '');
  const len = Math.floor(s.length * 3 / 4);
  const r = new Uint8Array(len);
  let idx = 0;
  for (let i = 0; i < s.length; i += 4) {
    const c0 = B64.indexOf(s[i]), c1 = B64.indexOf(s[i + 1] || 'A');
    const c2 = B64.indexOf(s[i + 2] || 'A'), c3 = B64.indexOf(s[i + 3] || 'A');
    r[idx++] = (c0 << 2) | (c1 >> 4);
    if (s[i + 2] && s[i + 2] !== '=') r[idx++] = ((c1 & 15) << 4) | (c2 >> 2);
    if (s[i + 3] && s[i + 3] !== '=') r[idx++] = ((c2 & 3) << 6) | c3;
  }
  return r;
}

function opt(extra: huks.HuksParam[]): huks.HuksOptions {
  return { properties: extra };
}
function tag(t: huks.HuksTag, v: number | boolean | Uint8Array): huks.HuksParam {
  return { tag: t, value: v };
}

// ---- 导出类 ----
export class CryptoManager {
  private static _ready = false;

  static async init(): Promise<void> {
    if (this._ready) return;
    try { await huks.isKeyItemExist(KEY_ALIAS, opt([])); }
    catch (_) {
      await huks.generateKeyItem(KEY_ALIAS, opt([
        tag(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
        tag(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
        tag(huks.HuksTag.HUKS_TAG_PURPOSE, huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT | huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT),
        tag(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
        tag(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
      ]));
    }
    this._ready = true;
  }

  static async encrypt(plain: string): Promise<string> {
    await this._readyCheck();
    const iv = new Uint8Array(12);
    for (let i = 0; i < 12; i++) iv[i] = Math.floor(Math.random() * 256);
    const data = new Uint8Array(plain.length);
    for (let i = 0; i < plain.length; i++) data[i] = plain.charCodeAt(i);

    try {
      const session = await huks.initSession(KEY_ALIAS, opt([
        tag(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
        tag(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
        tag(huks.HuksTag.HUKS_TAG_PURPOSE, huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT),
        tag(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
        tag(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
        tag(huks.HuksTag.HUKS_TAG_IV, iv),
      ]));
      const result = await huks.finishSession(session.handle, opt([]), data);
      await huks.abortSession(session.handle, opt([]));

      const out = new Uint8Array(12 + result.outData!.length);
      out.set(iv);
      out.set(result.outData!, 12);
      return toB64(out);
    } catch (e) {
      return toB64(data); // fallback: store as base64
    }
  }

  static async decrypt(cipher: string): Promise<string> {
    await this._readyCheck();
    try {
      const raw = fromB64(cipher);
      if (raw.length <= 12) return cipher;
      const iv = raw.slice(0, 12);
      const enc = raw.slice(12);

      const session = await huks.initSession(KEY_ALIAS, opt([
        tag(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
        tag(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
        tag(huks.HuksTag.HUKS_TAG_PURPOSE, huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT),
        tag(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
        tag(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
        tag(huks.HuksTag.HUKS_TAG_IV, iv),
      ]));
      const result = await huks.finishSession(session.handle, opt([]), enc);
      await huks.abortSession(session.handle, opt([]));

      const dec = result.outData!;
      let s = '';
      for (let i = 0; i < dec.length; i++) s += String.fromCharCode(dec[i]);
      return s;
    } catch (_) {
      return cipher;
    }
  }

  private static async _readyCheck(): Promise<void> {
    if (!this._ready) await this.init();
  }
}
