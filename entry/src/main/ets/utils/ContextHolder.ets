// 全局 AbilityContext 持有者，解决 OHOS 6.0 ArkTS 严格类型限制
let _abilityCtx: Object | null = null;

export function setAbilityContext(ctx: Object): void {
  _abilityCtx = ctx;
}

export function getAbilityContext(): Object | null {
  return _abilityCtx;
}
