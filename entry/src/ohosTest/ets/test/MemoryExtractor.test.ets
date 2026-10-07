import { describe, it, expect } from '@ohos/hypium';
import {
  extractStructuredMemory,
  StructuredMemoryExtraction
} from '../../../main/ets/utils/MemoryExtractor';

export default function memoryExtractorTest(): void {
  describe('MemoryExtractorTest', (): void => {
    it('extractsNoPunctuationPreference', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('我喜欢跑步');
      expect(result.liked.join('、')).assertEqual('跑步');
      expect(result.disliked.length).assertEqual(0);
    });

    it('splitsAndDeduplicatesPreferences', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('我喜欢跑步和游泳、跑步');
      expect(result.liked.join('、')).assertEqual('跑步、游泳');
    });

    it('rejectsPreferenceQuestion', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('你记得我喜欢什么吗');
      expect(result.liked.length).assertEqual(0);
      expect(result.disliked.length).assertEqual(0);
    });

    it('extractsPreferenceRevocation', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('我现在不喜欢跑步了');
      expect(result.disliked.join('、')).assertEqual('跑步');
      expect(result.liked.length).assertEqual(0);
    });

    it('handlesOmittedSubjectAfterConjunction', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('我喜欢跑步但是不喜欢游泳');
      expect(result.liked.join('、')).assertEqual('跑步');
      expect(result.disliked.join('、')).assertEqual('游泳');
    });

    it('extractsAdjacentStructuredFields', 0, (): void => {
      const result: StructuredMemoryExtraction = extractStructuredMemory('我叫小明我来自成都生活');
      expect(result.fields.length).assertEqual(2);
      expect(result.fields[0].key).assertEqual('称呼');
      expect(result.fields[0].value).assertEqual('小明');
      expect(result.fields[1].key).assertEqual('所在地');
      expect(result.fields[1].value).assertEqual('成都');
    });
  });
}
