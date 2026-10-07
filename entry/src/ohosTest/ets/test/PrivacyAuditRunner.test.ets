import { describe, it, expect } from '@ohos/hypium';
import { isAuthenticatedEnvelopeHeader } from '../../../main/ets/utils/PrivacyAuditRunner';

export default function privacyAuditRunnerTest(): void {
  describe('PrivacyAuditRunnerTest', (): void => {
    it('acceptsAuthenticatedV2Header', 0, (): void => {
      const bytes: Uint8Array = new Uint8Array([79, 72, 79, 83, 69, 78, 67, 50, 58]);
      expect(isAuthenticatedEnvelopeHeader(bytes, bytes.length)).assertTrue();
    });

    it('rejectsPlainJsonHeader', 0, (): void => {
      const bytes: Uint8Array = new Uint8Array([123, 34, 102, 97, 99, 116, 115, 34, 58]);
      expect(isAuthenticatedEnvelopeHeader(bytes, bytes.length)).assertFalse();
    });

    it('rejectsTruncatedHeader', 0, (): void => {
      const bytes: Uint8Array = new Uint8Array([79, 72, 79, 83]);
      expect(isAuthenticatedEnvelopeHeader(bytes, bytes.length)).assertFalse();
    });
  });
}
