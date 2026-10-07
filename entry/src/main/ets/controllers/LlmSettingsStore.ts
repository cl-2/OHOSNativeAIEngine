/** Encrypted persistence and pure validation for LLM routing settings. */
import { EncryptedStore } from '../utils/EncryptedStore';

export interface StoredRemoteLlmConfig {
  providerIndex: number;
  apiUrl: string;
  modelName: string;
  apiKey: string;
}

export interface StoredRoutingMode {
  mode: number;
}

export function isRemoteConfigComplete(config: StoredRemoteLlmConfig, isOllama: boolean): boolean {
  if (!config.apiUrl.trim() || !config.modelName.trim()) return false;
  return isOllama || config.apiKey.trim().length > 0;
}

export function normalizeRemoteUrl(apiUrl: string, isOllama: boolean): string {
  let url: string = apiUrl.trim();
  if (!isOllama || !url) return url;
  if (!url.startsWith('http://') && !url.startsWith('https://')) url = 'http://' + url;
  const match: RegExpMatchArray | null = url.match(/^(https?:\/\/[^/]+)(\/.*)?$/);
  if (!match) return url;
  let origin: string = match[1];
  const path: string = match[2] || '';
  const host: string = origin.substring(origin.indexOf('://') + 3);
  if (!host.includes(':')) origin += ':11434';
  if (!path || path === '/') return origin + '/v1/chat/completions';
  return origin + path;
}

export class LlmSettingsStore {
  private readonly remoteConfigPath: string;
  private readonly routingModePath: string;

  constructor(filesDir: string) {
    this.remoteConfigPath = filesDir + '/llm/remote_llm_config.enc';
    this.routingModePath = filesDir + '/llm/llm_routing_mode.enc';
  }

  loadRemoteConfig(): Promise<StoredRemoteLlmConfig | null> {
    return EncryptedStore.read<StoredRemoteLlmConfig>(this.remoteConfigPath);
  }

  saveRemoteConfig(config: StoredRemoteLlmConfig): Promise<void> {
    return EncryptedStore.write(this.remoteConfigPath, config);
  }

  loadRoutingMode(): Promise<StoredRoutingMode | null> {
    return EncryptedStore.read<StoredRoutingMode>(this.routingModePath);
  }

  saveRoutingMode(mode: number): Promise<void> {
    const config: StoredRoutingMode = { mode };
    return EncryptedStore.write(this.routingModePath, config);
  }
}
