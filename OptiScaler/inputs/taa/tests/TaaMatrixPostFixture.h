// Exact captured PS6CA/VSF1CE, with an independent high-resolution GPU oracle.
#pragma once
struct TaaMatrixPostFixture {
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> vertices,vertexCb,pixelCb;
    std::array<ComPtr<ID3D11Texture2D>,4> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> oracle;ComPtr<ID3D11RenderTargetView> oracleRtv;
    std::array<float,435*4> constants{};
    void Init(ID3D11Device* device,unsigned size,DXGI_FORMAT format) {
        const auto load=[](const char* file){std::ifstream in(file,std::ios::binary);std::vector<char> out((std::istreambuf_iterator<char>(in)),{});Check(!out.empty(),"matrix captured bytecode");return out;};
        auto v=load("matrix.vs.dxbc"),p=load("matrix.ps.dxbc");
        Check(DrawObservationDx11::ShaderByteHash(v.data(),v.size())==0xF1CE86BC0BE8E994ull&&DrawObservationDx11::ShaderByteHash(p.data(),p.size())==0x6CA04A04AFEE2570ull,"actual matrix/postprocess byte identity");
        Hr(device->CreateVertexShader(v.data(),v.size(),nullptr,&vs));Hr(device->CreatePixelShader(p.data(),p.size(),nullptr,&ps));
        const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};Hr(device->CreateInputLayout(&element,1,v.data(),v.size(),&layout));
        const float points[]{-1,-1,3,-1,-1,3};D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(points);b.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{points,0,0};Hr(device->CreateBuffer(&b,&init,&vertices));
        float matrix[64]{};matrix[12*4]=1;matrix[13*4+1]=-1;matrix[15*4+2]=.5f;matrix[15*4+3]=1;
        b.ByteWidth=sizeof(matrix);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;init.pSysMem=matrix;Hr(device->CreateBuffer(&b,&init,&vertexCb));
        b.ByteWidth=sizeof(constants);Hr(device->CreateBuffer(&b,nullptr,&pixelCb));
        constants[60*4+1]=1;constants[70*4]=1.0f/1024;constants[70*4+1]=1.0f/32;constants[70*4+2]=31;
        constants[138*4]=.1f;constants[145*4]=.6f;constants[433*4]=constants[433*4+1]=constants[433*4+2]=.8f;constants[433*4+3]=1;
        constants[434*4]=constants[434*4+1]=.5f;constants[434*4+2]=1;constants[434*4+3]=1;
        const unsigned widths[]{80,4,1024,256},heights[]{50,4,32,256};
        for(unsigned i=0;i<4;++i){D3D11_TEXTURE2D_DESC d{};d.Width=widths[i];d.Height=heights[i];d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            std::vector<float> pixels(size_t(d.Width)*d.Height*4,.01f);
            if(i==0)std::fill(pixels.begin(),pixels.end(),.2f);
            if(i==2)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<1024;++x){const auto at=(size_t(y)*1024+x)*4;pixels[at]=float(x%32)/31;pixels[at+1]=float(y)/31;pixels[at+2]=float(x/32)/31;pixels[at+3]=1;}
            D3D11_SUBRESOURCE_DATA data{pixels.data(),d.Width*16,0};Hr(device->CreateTexture2D(&d,&data,&textures[i]));Hr(device->CreateShaderResourceView(textures[i].Get(),nullptr,&views[i]));}
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Hr(device->CreateSamplerState(&sd,&sampler));
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET;Hr(device->CreateTexture2D(&d,nullptr,&oracle));Hr(device->CreateRenderTargetView(oracle.Get(),nullptr,&oracleRtv));
    }
    void Update(ID3D11DeviceContext* context,unsigned frame) {
        // Distinct current-frame bloom strength catches stale copied constants.
        constants[145*4]=(frame&1)?.6f:.05f;
        context->UpdateSubresource(pixelCb.Get(),0,nullptr,constants.data(),0,0);
        float matrix[64]{};matrix[48]=1;matrix[53]=-1;matrix[62]=.5f;matrix[63]=1;
        context->UpdateSubresource(vertexCb.Get(),0,nullptr,matrix,0,0);
    }
    void Bind(ID3D11DeviceContext* context) {
        context->IASetInputLayout(layout.Get());ID3D11Buffer* vb=vertices.Get();UINT stride=8,offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer* a=vertexCb.Get();context->VSSetConstantBuffers(0,1,&a);a=pixelCb.Get();context->PSSetConstantBuffers(0,1,&a);
        ID3D11ShaderResourceView* inputs[]{views[0].Get(),views[1].Get(),views[2].Get(),views[3].Get()};context->PSSetShaderResources(1,4,inputs);
        ID3D11SamplerState* states[]{sampler.Get(),sampler.Get()};context->PSSetSamplers(0,2,states);
    }
};
