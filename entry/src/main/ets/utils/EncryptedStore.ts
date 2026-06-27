/**
 * EncryptedStore.ts - 加密持久化存储
 *
 * 透明地对 JSON 文件做 AES-256-GCM 加解密。
 * 兼容旧数据：读取时自动检测加密状态。
 */

import { fileIo } from '@kit.CoreFileKit';
import { CryptoManager } from './CryptoManager';

export class EncryptedStore {

  /**
   * 写入加密的 JSON 对象
   */
  static async write(path: string, data: Object): Promise<void> {
    try {
      const dir: string = path.substring(0, path.lastIndexOf('/'));
      fileIo.mkdirSync(dir, true);
      const json: string = JSON.stringify(data);
      const encrypted: string = await CryptoManager.encrypt(json);
      const f = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(f.fd, encrypted);
      fileIo.closeSync(f);
    } catch (e) {
      console.error('OHOS_Store: write failed: ' + String(e));
    }
  }

  /**
   * 读取（自动识别加密/未加密）
   */
  static async read<T>(path: string): Promise<T | null> {
    try {
      const content: string = fileIo.readTextSync(path);
      if (!content) return null;
      // 先尝试直接解析（未加密）
      try { return JSON.parse(content) as T; } catch (_e) {}
      // 再尝试解密后解析
      const decrypted: string = await CryptoManager.decrypt(content);
      try { return JSON.parse(decrypted) as T; } catch (_e) { return null; }
    } catch (_e) {
      return null;
    }
  }
}
