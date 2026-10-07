/**
 * Versioned authenticated encryption backed by HarmonyOS HUKS.
 *
 * V2 envelope: "OHOSENC2:" + base64(nonce[12] || ciphertext || tag[16]).
 * New writes never fall back to reversible encoding when HUKS fails.
 */

import huks from '@ohos.security.huks';
import cryptoFramework from '@ohos.security.cryptoFramework';
import { util } from '@kit.ArkTS';

const LEGACY_KEY_ALIAS: string = 'ohos_ai_enc_key';
const V2_KEY_ALIAS: string = 'ohos_ai_enc_key_v2';
const V2_PREFIX: string = 'OHOSENC2:';
const NONCE_BYTES: number = 12;
const TAG_BYTES: number = 16;
const B64: string = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function options(properties: huks.HuksParam[], inData?: Uint8Array): huks.HuksOptions {
  const result: huks.HuksOptions = { properties };
  if (inData !== undefined) result.inData = inData;
  return result;
}

function param(tagValue: huks.HuksTag, value: number | boolean | Uint8Array): huks.HuksParam {
  return { tag: tagValue, value };
}

function toBase64(bytes: Uint8Array): string {
  let result: string = '';
  for (let i: number = 0; i < bytes.length; i += 3) {
    const b0: number = bytes[i];
    const hasB1: boolean = i + 1 < bytes.length;
    const hasB2: boolean = i + 2 < bytes.length;
    const b1: number = hasB1 ? bytes[i + 1] : 0;
    const b2: number = hasB2 ? bytes[i + 2] : 0;
    result += B64[b0 >> 2];
    result += B64[((b0 & 3) << 4) | (b1 >> 4)];
    result += hasB1 ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    result += hasB2 ? B64[b2 & 63] : '=';
  }
  return result;
}

function fromBase64(value: string): Uint8Array {
  const input: string = value.trim();
  if (!input || input.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(input)) {
    throw new Error('invalid encrypted base64 payload');
  }
  let padding: number = 0;
  if (input.endsWith('==')) padding = 2;
  else if (input.endsWith('=')) padding = 1;
  const output: Uint8Array = new Uint8Array(input.length / 4 * 3 - padding);
  let offset: number = 0;
  for (let i: number = 0; i < input.length; i += 4) {
    const c0: number = B64.indexOf(input[i]);
    const c1: number = B64.indexOf(input[i + 1]);
    const c2: number = input[i + 2] === '=' ? 0 : B64.indexOf(input[i + 2]);
    const c3: number = input[i + 3] === '=' ? 0 : B64.indexOf(input[i + 3]);
    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) throw new Error('invalid encrypted base64 character');
    if (offset < output.length) output[offset++] = (c0 << 2) | (c1 >> 4);
    if (offset < output.length) output[offset++] = ((c1 & 15) << 4) | (c2 >> 2);
    if (offset < output.length) output[offset++] = ((c2 & 3) << 6) | c3;
  }
  return output;
}

function concat(first: Uint8Array, second: Uint8Array): Uint8Array {
  const result: Uint8Array = new Uint8Array(first.length + second.length);
  result.set(first, 0);
  result.set(second, first.length);
  return result;
}

function utf8Encode(value: string): Uint8Array {
  return util.TextEncoder.create('utf-8').encodeInto(value);
}

function utf8Decode(value: Uint8Array): string {
  return util.TextDecoder.create('utf-8', { fatal: true }).decodeToString(value);
}

export class CryptoManager {
  private static readyPromise: Promise<void> | null = null;

  static init(): Promise<void> {
    if (!this.readyPromise) this.readyPromise = this.ensureV2Key();
    return this.readyPromise;
  }

  static isV2Envelope(value: string): boolean {
    return value.startsWith(V2_PREFIX);
  }

  static async encrypt(plain: string): Promise<string> {
    await this.init();
    const nonce: Uint8Array = cryptoFramework.createRandom().generateRandomSync(NONCE_BYTES).data;
    const properties: huks.HuksParam[] = this.cipherProperties(
      huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT, nonce
    );
    let handle: number = 0;
    let sessionOpened: boolean = false;
    try {
      const session: huks.HuksSessionHandle = await huks.initSession(V2_KEY_ALIAS, options(properties));
      handle = session.handle;
      sessionOpened = true;
      const result: huks.HuksReturnResult = await huks.finishSession(
        handle, options([], utf8Encode(plain))
      );
      sessionOpened = false;
      const encrypted: Uint8Array = result.outData || new Uint8Array(0);
      if (encrypted.length < TAG_BYTES) throw new Error('HUKS GCM output has no authentication tag');
      return V2_PREFIX + toBase64(concat(nonce, encrypted));
    } catch (e) {
      if (sessionOpened) {
        try { await huks.abortSession(handle, options([])); } catch (_) {}
      }
      throw new Error('HUKS encryption failed: ' + String(e));
    }
  }

  static async decrypt(cipher: string): Promise<string> {
    if (!this.isV2Envelope(cipher)) return this.decryptLegacy(cipher);
    await this.init();
    const payload: Uint8Array = fromBase64(cipher.substring(V2_PREFIX.length));
    if (payload.length <= NONCE_BYTES + TAG_BYTES) throw new Error('encrypted payload is too short');
    const nonce: Uint8Array = payload.slice(0, NONCE_BYTES);
    const cipherAndTag: Uint8Array = payload.slice(NONCE_BYTES);
    const encrypted: Uint8Array = cipherAndTag.slice(0, cipherAndTag.length - TAG_BYTES);
    const authTag: Uint8Array = cipherAndTag.slice(cipherAndTag.length - TAG_BYTES);
    const properties: huks.HuksParam[] = this.cipherProperties(
      huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT, nonce, authTag
    );
    let handle: number = 0;
    let sessionOpened: boolean = false;
    try {
      const session: huks.HuksSessionHandle = await huks.initSession(V2_KEY_ALIAS, options(properties));
      handle = session.handle;
      sessionOpened = true;
      const result: huks.HuksReturnResult = await huks.finishSession(handle, options([], encrypted));
      sessionOpened = false;
      return utf8Decode(result.outData || new Uint8Array(0));
    } catch (e) {
      if (sessionOpened) {
        try { await huks.abortSession(handle, options([])); } catch (_) {}
      }
      throw new Error('HUKS decryption/authentication failed: ' + String(e));
    }
  }

  /** Recovers legacy Base64 fallback or the old IV+cipher envelope for one-time migration. */
  static async decryptLegacy(cipher: string): Promise<string> {
    const raw: Uint8Array = fromBase64(cipher);
    try {
      const decoded: string = utf8Decode(raw);
      if (decoded.startsWith('{') || decoded.startsWith('[')) return decoded;
    } catch (_) {}
    if (raw.length <= NONCE_BYTES) throw new Error('unsupported legacy encrypted payload');
    const iv: Uint8Array = raw.slice(0, NONCE_BYTES);
    const encrypted: Uint8Array = raw.slice(NONCE_BYTES);
    const properties: huks.HuksParam[] = [
      param(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
      param(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
      param(huks.HuksTag.HUKS_TAG_PURPOSE, huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT),
      param(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
      param(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
      param(huks.HuksTag.HUKS_TAG_IV, iv)
    ];
    let handle: number = 0;
    let sessionOpened: boolean = false;
    try {
      const session: huks.HuksSessionHandle = await huks.initSession(LEGACY_KEY_ALIAS, options(properties));
      handle = session.handle;
      sessionOpened = true;
      const result: huks.HuksReturnResult = await huks.finishSession(handle, options([], encrypted));
      sessionOpened = false;
      return utf8Decode(result.outData || new Uint8Array(0));
    } catch (e) {
      if (sessionOpened) {
        try { await huks.abortSession(handle, options([])); } catch (_) {}
      }
      throw new Error('legacy encrypted payload cannot be recovered: ' + String(e));
    }
  }

  static async selfTest(): Promise<boolean> {
    const sample: string = '端侧加密自检-中文-123';
    const encrypted: string = await this.encrypt(sample);
    if (await this.decrypt(encrypted) !== sample) return false;
    const index: number = V2_PREFIX.length + Math.floor((encrypted.length - V2_PREFIX.length) / 2);
    const original: string = encrypted[index];
    const changed: string = original === 'A' ? 'B' : 'A';
    const tampered: string = encrypted.substring(0, index) + changed + encrypted.substring(index + 1);
    try {
      await this.decrypt(tampered);
      return false;
    } catch (_) {
      return true;
    }
  }

  private static cipherProperties(purpose: huks.HuksKeyPurpose, nonce: Uint8Array,
    authTag?: Uint8Array): huks.HuksParam[] {
    const properties: huks.HuksParam[] = [
      param(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
      param(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
      param(huks.HuksTag.HUKS_TAG_PURPOSE, purpose),
      param(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
      param(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
      param(huks.HuksTag.HUKS_TAG_BLOCK_MODE, huks.HuksCipherMode.HUKS_MODE_GCM),
      param(huks.HuksTag.HUKS_TAG_NONCE, nonce)
    ];
    if (authTag !== undefined) properties.push(param(huks.HuksTag.HUKS_TAG_AE_TAG, authTag));
    return properties;
  }

  private static async ensureV2Key(): Promise<void> {
    try {
      const exists: boolean = await huks.isKeyItemExist(V2_KEY_ALIAS, options([]));
      if (exists) return;
    } catch (_) {}
    const properties: huks.HuksParam[] = [
      param(huks.HuksTag.HUKS_TAG_ALGORITHM, huks.HuksKeyAlg.HUKS_ALG_AES),
      param(huks.HuksTag.HUKS_TAG_KEY_SIZE, huks.HuksKeySize.HUKS_AES_KEY_SIZE_256),
      param(huks.HuksTag.HUKS_TAG_PURPOSE,
        huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT | huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT),
      param(huks.HuksTag.HUKS_TAG_DIGEST, huks.HuksKeyDigest.HUKS_DIGEST_NONE),
      param(huks.HuksTag.HUKS_TAG_PADDING, huks.HuksKeyPadding.HUKS_PADDING_NONE),
      param(huks.HuksTag.HUKS_TAG_BLOCK_MODE, huks.HuksCipherMode.HUKS_MODE_GCM)
    ];
    await huks.generateKeyItem(V2_KEY_ALIAS, options(properties));
  }
}
