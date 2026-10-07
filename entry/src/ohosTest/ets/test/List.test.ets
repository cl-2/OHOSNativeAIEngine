import abilityTest from './Ability.test';
import memoryExtractorTest from './MemoryExtractor.test';
import privacyAuditRunnerTest from './PrivacyAuditRunner.test';
import arktsControllerTest from './ArktsController.test';

export default function testsuite() {
  abilityTest();
  memoryExtractorTest();
  privacyAuditRunnerTest();
  arktsControllerTest();
}
