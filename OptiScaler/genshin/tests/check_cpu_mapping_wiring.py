"""Structural checks for the shared CPU mapping implementation; no GPU proof."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
semantic = (ROOT / 'genshin/GenshinDepthSemantic.h').read_text(encoding='utf-8-sig')
portable = (ROOT / 'genshin/GenshinDepthPortableResources.h').read_text(encoding='utf-8-sig')
returned = (ROOT / 'genshin/GenshinReturnedRaster.h').read_text(encoding='utf-8-sig')
hooks = (ROOT / 'resource_tracking/GenericDepth_Dx11.cpp').read_text(encoding='utf-8-sig')
checks = 0

def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)

check('MappedResourceJournal<CpuMap,32>' in semantic, 'semantic maps use fixed shared journal')
check('MappedResourceJournal<Map,32,FramePipeline::MapConflict::Resource>' in portable, 'portable maps preserve resource conflict policy')
check('struct CpuMaps {' not in semantic and 'struct Maps {' not in portable, 'duplicate registries removed')
check('FramePipeline::CpuSnapshotBytes bytes' in semantic and 'FramePipeline::CpuSnapshotBytes data' in portable, 'shared exact CPU storage in both adapters')
check('shadowRetention' not in semantic, 'one payload allocation/retention for dynamic semantic shadow')
unmap = semantic.split('static Submission Unmap(', 1)[1].split('// A successful receipt', 1)[0]
check(unmap.index('std::memcpy(out.mapped_.driver') < unmap.index('detail::Access a;'), 'driver writeback precedes owner/loss admission')
check('if(exact){auto payload=out.mapped_.shadow;' in unmap, 'publish the original shadow without a second copy')
check('NewPayload(' not in unmap, 'no second snapshot allocation on Unmap')
for contract in ('out.mapped_.thread==GetCurrentThreadId()', 'out.mapped_.revision==b->revision',
                 'out.mapped_.loss==a.s.lossEpoch', 'a.s.lossEpoch==detail::loss.load()'):
    check(contract in unmap, 'retain exact Unmap condition: ' + contract)
published = semantic.split('bool Returned()noexcept {', 1)[1].split('inline std::shared_ptr<const Draw> Capture(', 1)[0]
check('revision_==b->revision' in published and 'b->payload=payload_' in published, 'publication only in actual returned boundary')
check('boundary==Boundary::Map' in returned, 'raster boundary owns Map invalidation')
mapped = hooks.split('HRESULT STDMETHODCALLTYPE hkDepthMap(', 1)[1].split('__declspec(noinline) void STDMETHODCALLTYPE hkDepthUnmap', 1)[0]
check('GenshinDepthSemantic::Invalidate(c,r,true)' not in mapped, 'remove duplicate semantic Map invalidation')
check('preserving && !sub' in mapped, 'preimage lookup only for preserving maps')
check(mapped.index('GenshinCpuWriterProbe::Mapped') > mapped.index('GenshinDepthPortable::Mapped'), 'probe sees application-returned CPU pointer')
replay = (ROOT / 'genshin/GenshinDepthReplayState.h').read_text(encoding='utf-8-sig')
check('expanded->data.get()' not in replay and 'p.storage->data.get()' not in replay,
      'replay consumers use the shared snapshot byte interface')
print(f'CPU mapping wiring: {checks} checks passed')
