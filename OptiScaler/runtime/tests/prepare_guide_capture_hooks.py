"""Extract actual public submission wrappers for the WARP integration fixture."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[3]
source = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Late.inl').read_text(encoding='utf-8-sig')
def body(signature):
    begin = source.index(signature)
    cursor = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[begin:cursor]
pre = body('GuideCapture::ExecutionReceipt FinishedPictureSubmitting(')
post = body('void FinishedPictureSubmitted(')
nr = body('static void FinishedPictureSubmittedReal(')
assert 'GuideCapture::Submitted(' not in nr
assert post.index('RealSubmittedCommands(') < post.index('GuideCapture::Submitted(') < post.index('FinishedPictureSubmittedReal(')
assert 'GuideCapture::Executing(' in pre
assert 'return receipt;' in pre
assert 'receipt.preview' in pre and 'receipt.offline' in pre
assert 'const GuideCapture::ExecutionReceipt& receipt' in post
assert 'GuideCapture::Submitted(queue,batch,realLists.data(),receipt)' in ''.join(post.split())
target = Path(sys.argv[1])
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(pre + '\n' + post + '\n', encoding='utf-8')
print('Actual canonical sampler hooks extracted; NR watcher gate remains separate')
