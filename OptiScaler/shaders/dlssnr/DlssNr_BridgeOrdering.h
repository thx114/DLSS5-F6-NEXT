#pragma once

namespace DlssNr
{
// A successful bridge output handoff queues the producer->DX11 wait, and the
// game-facing Present then queues DX11->copy->present waits. Queue identity
// alone is insufficient; native presents never supply this completed chain.
inline bool BridgeProducerOrdered(bool bridgeCapture, bool outputHandoffQueued,
                                  bool presentWaitChainQueued, bool sameProducer)
{
    return bridgeCapture && outputHandoffQueued && presentWaitChainQueued && sameProducer;
}
}
