"""Execute the production deferred seam; only external GPU/NGX operands are faked."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[5]
source = Path(sys.argv[1]) if len(sys.argv)>1 else root/'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'
out = Path(sys.argv[2]) if len(sys.argv)>2 else root/'build/common-core-tests'
out.mkdir(parents=True,exist_ok=True)
text=source.read_text(encoding='utf-8')
def block(needle):
    start=text.index(needle); opening=text.index('{',start); depth=1; end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]
(out/'deferred-pending.inl').write_text(block('struct Pending')+' pending;\n',encoding='utf-8')
before=block('void Before(')
(out/'deferred-before.inl').write_text(before[before.index('    DlssNrFrameInfo frame {};'):-1],encoding='utf-8')
(out/'deferred-after.inl').write_text(block('void After(')+'\n',encoding='utf-8')
print('Extracted production Pending, colour-selection/arm tail, and complete After:',source)
