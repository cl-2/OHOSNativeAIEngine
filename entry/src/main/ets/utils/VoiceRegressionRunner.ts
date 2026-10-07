import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';
import {
  decideLlmRoute,
  LlmRouteContext,
  LlmRouteDecision,
  ROUTING_LOCAL,
  ROUTING_REMOTE,
  ROUTING_SMART
} from './LlmRoutePolicy';
import {
  decideLocalSkill,
  LocalSkillDecision,
  LocalSkillVector,
  localSkillRegressionVectors
} from './LocalSkillRouter';
import {
  parseReminderCommand,
  ReminderCommand,
  ReminderParserVector,
  reminderParserRegressionVectors
} from './ReminderParser';
import { MemoCommand, MemoParserVector, memoParserRegressionVectors, parseMemoCommand } from './MemoParser';
import {
  DeterministicSkillRoute,
  DeterministicSkillRouteVector,
  deterministicSkillRouteVectors,
  routeDeterministicSkill
} from './DeterministicSkillRouter';

export interface VoiceRegressionCase {
  name: string;
  passed: boolean;
  detail: string;
}

interface NativeVoiceRegressionReport {
  suite: string;
  timestampMs: number;
  passed: number;
  failed: number;
  cases: VoiceRegressionCase[];
}

interface RouteVector {
  name: string;
  context: LlmRouteContext;
  expectedTarget: string;
  reasonContains: string;
}

export interface VoiceRegressionReport {
  schemaVersion: number;
  startedAt: number;
  finishedAt: number;
  success: boolean;
  passed: number;
  failed: number;
  nativeCases: VoiceRegressionCase[];
  routeCases: VoiceRegressionCase[];
  skillCases: VoiceRegressionCase[];
  error: string;
  scope: string;
  persisted: boolean;
  persistError: string;
}

const REMOTE_LABEL: string = '☁️ 回归远端';

export class VoiceRegressionRunner {
  private static baseDir: string = '';
  private static running: boolean = false;
  private static historyBaseDir: string = '';
  private static history: VoiceRegressionReport[] = [];

  static init(filesDir: string): void {
    this.baseDir = filesDir;
    if (this.historyBaseDir !== filesDir) {
      this.historyBaseDir = filesDir;
      this.history = this.loadHistory(filesDir + '/perf/voice_regressions.json');
    }
  }

  static getHistory(): VoiceRegressionReport[] {
    if (!this.baseDir) return [];
    return this.history.slice();
  }

  static run(): VoiceRegressionReport {
    if (this.running) throw new Error('语音逻辑回归正在运行');
    if (!this.baseDir) throw new Error('语音逻辑回归尚未初始化');
    this.running = true;
    const report: VoiceRegressionReport = {
      schemaVersion: 5,
      startedAt: Date.now(),
      finishedAt: 0,
      success: false,
      passed: 0,
      failed: 0,
      nativeCases: [],
      routeCases: [],
      skillCases: [],
      error: '',
      scope: '确定性逻辑：ASR语气词过滤、对话状态机、打断握手、隐私路由、本地指令路由、中文提醒与备忘解析',
      persisted: false,
      persistError: ''
    };
    try {
      const nativeRaw: string = nativeLib.runVoiceLogicRegression() as string;
      const nativeReport: NativeVoiceRegressionReport = JSON.parse(nativeRaw) as NativeVoiceRegressionReport;
      report.nativeCases = nativeReport.cases;
      report.routeCases = this.runRouteVectors();
      report.skillCases = this.runSkillVectors();
      const routePassed: number = this.countPassed(report.routeCases);
      const skillPassed: number = this.countPassed(report.skillCases);
      report.passed = nativeReport.passed + routePassed + skillPassed;
      report.failed = nativeReport.failed + report.routeCases.length - routePassed +
        report.skillCases.length - skillPassed;
      report.success = report.failed === 0;
    } catch (e) {
      report.error = String(e);
      report.failed++;
    } finally {
      report.finishedAt = Date.now();
      this.persist(report);
      this.running = false;
    }
    console.info('OHOS_REGRESSION voice logic passed=' + report.passed + ' failed=' + report.failed);
    return report;
  }

  private static makeContext(mode: number, userText: string, localInstalled: boolean,
    online: boolean, remoteConfigured: boolean, remoteCoolingDown: boolean = false): LlmRouteContext {
    return {
      mode,
      userText,
      localInstalled,
      online,
      remoteConfigured,
      remoteCoolingDown,
      remoteSourceLabel: REMOTE_LABEL
    };
  }

  private static runRouteVectors(): VoiceRegressionCase[] {
    const vectors: RouteVector[] = [
      { name: 'explicit_local_overrides_fact', context: this.makeContext(ROUTING_LOCAL, '某歌手简介', true, true, true), expectedTarget: 'local', reasonContains: '用户指定纯本地' },
      { name: 'explicit_remote', context: this.makeContext(ROUTING_REMOTE, '我的病历', true, true, true), expectedTarget: 'remote', reasonContains: '用户指定远端' },
      { name: 'privacy_stays_local', context: this.makeContext(ROUTING_SMART, '总结我的病历', true, true, true), expectedTarget: 'local', reasonContains: '隐私内容不上传' },
      { name: 'privacy_without_local_refuses', context: this.makeContext(ROUTING_SMART, '我的手机号是什么', false, true, true), expectedTarget: 'refuse', reasonContains: '隐私内容' },
      { name: 'device_task_local', context: this.makeContext(ROUTING_SMART, '调高音量', true, true, true), expectedTarget: 'local', reasonContains: '本地任务' },
      { name: 'fresh_fact_refuses_guess', context: this.makeContext(ROUTING_SMART, '今天天气怎么样', true, true, true), expectedTarget: 'refuse', reasonContains: '时效事实' },
      { name: 'factual_uses_remote', context: this.makeContext(ROUTING_SMART, '某歌手简介', true, true, true), expectedTarget: 'remote', reasonContains: '事实查询' },
      { name: 'factual_remote_cooldown_refuses', context: this.makeContext(ROUTING_SMART, '某歌手简介', true, true, true, true), expectedTarget: 'refuse', reasonContains: '事实无法核实' },
      { name: 'generic_prefers_local', context: this.makeContext(ROUTING_SMART, '写一句祝福', true, true, true), expectedTarget: 'local', reasonContains: '普通任务优先本地' },
      { name: 'generic_falls_back_remote', context: this.makeContext(ROUTING_SMART, '写一句祝福', false, true, true), expectedTarget: 'remote', reasonContains: '本地模型不可用' },
      { name: 'no_model_refuses', context: this.makeContext(ROUTING_SMART, '写一句祝福', false, false, false), expectedTarget: 'refuse', reasonContains: '无可用模型' },
      { name: 'interrupted_context_not_classified', context: this.makeContext(ROUTING_SMART, '请讲个故事\n\n[AI 之前说到:今天新闻]', true, true, true), expectedTarget: 'local', reasonContains: '普通任务优先本地' }
    ];
    const cases: VoiceRegressionCase[] = [];
    for (let i: number = 0; i < vectors.length; i++) {
      const vector: RouteVector = vectors[i];
      const actual: LlmRouteDecision = decideLlmRoute(vector.context);
      const passed: boolean = actual.target === vector.expectedTarget && actual.reason.includes(vector.reasonContains);
      cases.push({
        name: 'route:' + vector.name,
        passed,
        detail: 'expected=' + vector.expectedTarget + ',actual=' + actual.target + ',reason=' + actual.reason
      });
    }
    return cases;
  }

  private static runSkillVectors(): VoiceRegressionCase[] {
    const vectors: LocalSkillVector[] = localSkillRegressionVectors();
    const cases: VoiceRegressionCase[] = [];
    for (let i: number = 0; i < vectors.length; i++) {
      const vector: LocalSkillVector = vectors[i];
      const actual: LocalSkillDecision = decideLocalSkill(vector.input);
      cases.push({
        name: 'skill:' + i.toString() + ':' + vector.expected,
        passed: actual.skill === vector.expected,
        detail: 'input=' + vector.input + ',expected=' + vector.expected + ',actual=' + actual.skill
      });
    }
    const reminderVectors: ReminderParserVector[] = reminderParserRegressionVectors();
    const fixedNow: number = new Date(2026, 7, 4, 10, 0, 0, 0).getTime();
    for (let i: number = 0; i < reminderVectors.length; i++) {
      const vector: ReminderParserVector = reminderVectors[i];
      const actual: ReminderCommand = parseReminderCommand(vector.input, fixedNow);
      cases.push({
        name: 'reminder:' + i.toString() + ':' + vector.expected,
        passed: actual.kind === vector.expected && (actual.kind !== 'create' || (!actual.error && actual.triggerAtMs > fixedNow)),
        detail: 'input=' + vector.input + ',expected=' + vector.expected + ',actual=' + actual.kind + ',error=' + actual.error
      });
    }
    const memoVectors: MemoParserVector[] = memoParserRegressionVectors();
    for (let i: number = 0; i < memoVectors.length; i++) {
      const vector: MemoParserVector = memoVectors[i];
      const actual: MemoCommand = parseMemoCommand(vector.input);
      cases.push({
        name: 'memo:' + i.toString() + ':' + vector.expected,
        passed: actual.kind === vector.expected && (actual.kind !== 'create' || (!actual.error && actual.content.length > 0)),
        detail: 'input=' + vector.input + ',expected=' + vector.expected + ',actual=' + actual.kind + ',error=' + actual.error
      });
    }
    const skillRouteVectors: DeterministicSkillRouteVector[] = deterministicSkillRouteVectors();
    for (let i: number = 0; i < skillRouteVectors.length; i++) {
      const vector: DeterministicSkillRouteVector = skillRouteVectors[i];
      const actual: DeterministicSkillRoute = routeDeterministicSkill(vector.input, fixedNow);
      cases.push({
        name: 'skill_route:' + i.toString() + ':' + vector.expected,
        passed: actual.domain === vector.expected,
        detail: 'input=' + vector.input + ',expected=' + vector.expected + ',actual=' + actual.domain
      });
    }
    return cases;
  }

  private static countPassed(cases: VoiceRegressionCase[]): number {
    let result: number = 0;
    for (let i: number = 0; i < cases.length; i++) if (cases[i].passed) result++;
    return result;
  }

  private static pathExists(path: string): boolean {
    try {
      return fileIo.accessSync(path);
    } catch (e) {
      const message: string = String(e).toLowerCase();
      // Some HarmonyOS API levels return false for ENOENT while others throw
      // BusinessError 13900002. Normalize both behaviours here.
      if (message.includes('no such file') || message.includes('13900002')) return false;
      throw e;
    }
  }

  private static persist(report: VoiceRegressionReport): void {
    this.history.push(report);
    if (this.history.length > 20) this.history = this.history.slice(-20);
    const perfDir: string = this.baseDir + '/perf';
    const path: string = perfDir + '/voice_regressions.json';
    try {
      if (!this.pathExists(perfDir)) {
        try {
          fileIo.mkdirSync(perfDir);
        } catch (mkdirError) {
          // A concurrent diagnostics export may have created it meanwhile.
          if (!this.pathExists(perfDir)) throw mkdirError;
        }
      }
      if (!this.pathExists(perfDir)) {
        throw new Error('perf directory was not created');
      }
      report.persisted = true;
      report.persistError = '';
      let mode: number = fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      if (!this.pathExists(path)) {
        mode |= fileIo.OpenMode.CREATE;
      }
      const file: fileIo.File = fileIo.openSync(
        path, mode
      );
      fileIo.writeSync(file.fd, JSON.stringify(this.history));
      fileIo.closeSync(file);
    } catch (e) {
      report.persisted = false;
      report.persistError = 'prepare/write ' + path + ' failed: ' + String(e);
      console.warn('OHOS_REGRESSION persist failed=' + report.persistError);
    }
  }

  private static loadHistory(path: string): VoiceRegressionReport[] {
    try { return JSON.parse(fileIo.readTextSync(path)) as VoiceRegressionReport[]; } catch (_) { return []; }
  }
}
