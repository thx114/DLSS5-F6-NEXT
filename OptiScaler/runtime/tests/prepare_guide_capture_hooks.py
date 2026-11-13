"""Extract actual public submission wrappers for the WARP integration fixture."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[3]
source = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Late.inl').read_text(encoding='utf-8-sig')
def body(signature, text=source):
    begin = text.index(signature)
    cursor = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[begin:cursor]
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
owner = (root / 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp').read_text(encoding='utf-8-sig')
owner += "\n" + "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(
    next(a / "OptiScaler/framegen/dlssg" for a in Path(__file__).resolve().parents
         if (a / "OptiScaler/framegen/dlssg").is_dir()).glob("DLSSG_Dx12_*.cpp")))
base = (root / 'OptiScaler/framegen/IFGFeature_Dx12.cpp').read_text(encoding='utf-8-sig')
poll = body('IFGFeature_Dx12::UIAllocatorStatus IFGFeature_Dx12::PollUIAllocator(', base)
poll = poll.replace('IFGFeature_Dx12::', 'DLSSG_Dx12::')
retire = body('void DLSSG_Dx12::ResetCompletedGuideRecordings(', owner)
target.with_name('GuidePreviewOwnerProduction.inl').write_text(poll + '\n' + retire + '\n', encoding='utf-8')
print('Actual canonical sampler hooks extracted; NR watcher gate remains separate')
