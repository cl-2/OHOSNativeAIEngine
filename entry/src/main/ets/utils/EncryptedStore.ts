/** Authenticated JSON file storage with transparent legacy migration. */

import { fileIo } from '@kit.CoreFileKit';
import { CryptoManager } from './CryptoManager';

export class EncryptedStore {
  static async write(path: string, data: Object): Promise<void> {
    const dir: string = path.substring(0, path.lastIndexOf('/'));
    this.ensureDir(dir);
    const encrypted: string = await CryptoManager.encrypt(JSON.stringify(data));
    const tempPath: string = path + '.tmp';
    let file: fileIo.File | null = null;
    try {
      try { fileIo.unlinkSync(tempPath); } catch (_) {}
      file = fileIo.openSync(tempPath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      fileIo.writeSync(file.fd, encrypted);
      fileIo.closeSync(file);
      file = null;
      try { fileIo.unlinkSync(path); } catch (_) {}
      fileIo.renameSync(tempPath, path);
    } catch (e) {
      if (file !== null) {
        try { fileIo.closeSync(file); } catch (_) {}
      }
      try { fileIo.unlinkSync(tempPath); } catch (_) {}
      throw new Error('encrypted store write failed: ' + String(e));
    }
  }

  static async read<T>(path: string): Promise<T | null> {
    let content: string;
    try {
      content = fileIo.readTextSync(path);
    } catch (_) {
      return null;
    }
    if (!content) return null;

    // Plain JSON and legacy envelopes remain readable, but are rewritten once
    // with authenticated V2 encryption after successful parsing.
    if (!CryptoManager.isV2Envelope(content)) {
      let legacy: T | null = null;
      try {
        legacy = JSON.parse(content) as T;
      } catch (_) {
        try {
          legacy = JSON.parse(await CryptoManager.decryptLegacy(content)) as T;
        } catch (e) {
          console.error('OHOS_Store: legacy read failed=' + String(e));
          return null;
        }
      }
      if (legacy !== null) {
        try {
          await this.write(path, legacy as Object);
          console.info('OHOS_Store: migrated legacy file to authenticated V2');
        } catch (e) {
          console.error('OHOS_Store: migration failed, legacy data kept=' + String(e));
        }
      }
      return legacy;
    }

    try {
      return JSON.parse(await CryptoManager.decrypt(content)) as T;
    } catch (e) {
      console.error('OHOS_Store: authenticated read failed=' + String(e));
      return null;
    }
  }

  private static ensureDir(path: string): void {
    try {
      if (fileIo.accessSync(path)) return;
    } catch (_) {}
    try { fileIo.mkdirSync(path, true); } catch (e) {
      try {
        if (fileIo.accessSync(path)) return;
      } catch (_) {}
      throw e;
    }
  }
}
