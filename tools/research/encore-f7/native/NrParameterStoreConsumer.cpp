#include "NrParameterStoreConsumer.h"
#include <nvsdk_ngx_params.h>

namespace encore_research
{

void ConsumerSetAll(NVSDK_NGX_Parameter& parameters, const ConsumerValues& values)
{
    parameters.Set("consumer.ull", values.ull);
    parameters.Set("consumer.float", values.single);
    parameters.Set("consumer.double", values.real);
    parameters.Set("consumer.uint", values.uint);
    parameters.Set("consumer.int", values.integer);
    parameters.Set("consumer.d3d11", values.d3d11);
    parameters.Set("consumer.d3d12", values.d3d12);
    parameters.Set("consumer.pointer", values.opaque);
}

ConsumerResults ConsumerGetAll(const NVSDK_NGX_Parameter& parameters, ConsumerValues& outputs)
{
    ConsumerResults results;
    results.ull = parameters.Get("consumer.ull", &outputs.ull);
    results.single = parameters.Get("consumer.float", &outputs.single);
    results.real = parameters.Get("consumer.double", &outputs.real);
    results.uint = parameters.Get("consumer.uint", &outputs.uint);
    results.integer = parameters.Get("consumer.int", &outputs.integer);
    results.d3d11 = parameters.Get("consumer.d3d11", &outputs.d3d11);
    results.d3d12 = parameters.Get("consumer.d3d12", &outputs.d3d12);
    results.opaque = parameters.Get("consumer.pointer", &outputs.opaque);
    return results;
}

void ConsumerReset(NVSDK_NGX_Parameter& parameters)
{
    parameters.Reset();
}

} // namespace encore_research
