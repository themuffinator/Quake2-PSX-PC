/* Original GLSL translation of render/crt.c. Embedded so installs need no
 * external shader files. Logical row zero is at GL texel y=0 throughout;
 * only the final window blit flips it to the window's top-left convention. */
static const char gl_vertex[] =
    "#version 330 core\n"
    "void main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
    "gl_Position=vec4(p*2.0-1.0,0,1); }\n";

static const char gl_nearest[] =
    "#version 330 core\n"
    "uniform usampler2D Source; uniform ivec2 Size; out vec4 Colour;\n"
    "void main(){ ivec2 s=textureSize(Source,0);"
    "ivec2 p=clamp((ivec2(gl_FragCoord.xy)*2+1)*s/(Size*2),ivec2(0),s-1);"
    "uint v=texelFetch(Source,p,0).r;"
    "uvec3 rgb=(uvec3(v)>>uvec3(0,5,10))&31u;"
    "Colour=vec4(vec3((rgb<<3)|(rgb>>2))/255.0,1); }\n";

static const char gl_horizontal[] =
    "#version 330 core\n"
    "uniform usampler2D Source; uniform sampler2D Decode, Filtered; uniform bool UseFiltered;\n"
    "layout(location=0) out vec4 Beam; layout(location=1) out vec4 Blur;\n"
    "vec3 sampleLinear(ivec2 p){ p=clamp(p,ivec2(0),textureSize(Source,0)-1);"
    "if(UseFiltered)return texelFetch(Filtered,p,0).rgb;"
    "uint v=texelFetch(Source,p,0).r;"
    "return vec3(texelFetch(Decode,ivec2(int(v&31u),0),0).r,"
    "texelFetch(Decode,ivec2(int((v>>5)&31u),0),0).r,"
    "texelFetch(Decode,ivec2(int((v>>10)&31u),0),0).r); }\n"
    "void main(){ ivec2 p=ivec2(gl_FragCoord.xy);"
    "vec3 a=sampleLinear(p+ivec2(-2,0)), b=sampleLinear(p+ivec2(-1,0)),"
    "c=sampleLinear(p), d=sampleLinear(p+ivec2(1,0)), e=sampleLinear(p+ivec2(2,0));"
    "Beam=vec4(.76*c+.12*(b+d),1);"
    "Blur=vec4(.0625*a+.25*b+.375*c+.25*d+.0625*e,1); }\n";

static const char gl_vertical[] =
    "#version 330 core\n"
    "uniform sampler2D Beam, Blur; out vec4 Colour;\n"
    "vec3 sampleBlur(ivec2 p){return texelFetch(Blur,clamp(p,ivec2(0),"
    "textureSize(Blur,0)-1),0).rgb;}\n"
    "void main(){ ivec2 p=ivec2(gl_FragCoord.xy);"
    "vec3 glow=.0625*sampleBlur(p+ivec2(0,-2))+.25*sampleBlur(p+ivec2(0,-1))"
    "+.375*sampleBlur(p)+.25*sampleBlur(p+ivec2(0,1))+.0625*sampleBlur(p+ivec2(0,2));"
    "Colour=vec4(.94*texelFetch(Beam,p,0).rgb+.06*glow,1); }\n";

static const char gl_crt[] =
    "#version 330 core\n"
    "uniform sampler2D Linear, Profiles; uniform usampler2D Encode;"
    "uniform ivec2 Size; out vec4 Colour;\n"
    "vec3 samplePadded(ivec2 p){ivec2 s=textureSize(Linear,0);"
    "if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,s)))return vec3(0);"
    "return texelFetch(Linear,p,0).rgb;}\n"
    "void main(){ vec2 size=vec2(Size), source=vec2(textureSize(Linear,0));"
    "vec2 uv=gl_FragCoord.xy/size, q=uv*2.0-1.0;"
    "uv=.5+.5*q*(.985+.025*q.yx*q.yx);"
    "vec2 pos=clamp(uv*source-.5,vec2(-1),source-.001);"
    "ivec2 p=ivec2(floor(pos));"
    "vec2 f=floor(fract(pos)*65535.0)/65535.0;"
    "vec3 a=samplePadded(p), b=samplePadded(p+ivec2(1,0));"
    "vec3 c=samplePadded(p+ivec2(0,1)), d=samplePadded(p+ivec2(1,1));"
    "vec3 top=a+f.x*(b-a), bot=c+f.x*(d-c), rgb=top+f.y*(bot-top);"
    "int phase=int(fract(pos.y)*1024.0)%1024;"
    "vec2 beams=texelFetch(Profiles,ivec2(phase,0),0).rg;"
    "float luma=dot(rgb,vec3(.2126,.7152,.0722));"
    "float scan=(beams.x+(beams.y-beams.x)*sqrt(luma))/16384.0;"
    "vec2 edge=min(uv,1.0-uv)*size; q=uv*2.0-1.0;"
    "float shade=clamp(min(edge.x,edge.y)+.5,0.0,1.0)*(1.0-.065*dot(q,q));"
    "shade=floor(shade*65535.0)/65535.0;"
    "float mask=.12*clamp(size.x/source.x-1.0,0.0,1.0);"
    "vec3 phosphor=vec3(1.0-mask); phosphor[int(gl_FragCoord.x)%3]=1.0+mask*2.0;"
    "ivec3 level=ivec3(clamp(rgb*scan*shade*phosphor*8192.0+.5,0.0,8192.0));"
    "Colour=vec4(vec3(texelFetch(Encode,ivec2(level.r%128,level.r/128),0).r,"
    "texelFetch(Encode,ivec2(level.g%128,level.g/128),0).r,"
    "texelFetch(Encode,ivec2(level.b%128,level.b/128),0).r)/255.0,1); }\n";
/* Scene-only bloom, in linear light. Two Gaussian radii give a soft core and
 * broad halo without output-sized intermediate buffers. Every tap clamps to
 * its own viewport, preventing light from bleeding across split-screen gaps. */
#define BLOOM_REGION \
    "uniform ivec4 Views[4]; uniform int ViewCount;\n" \
    "ivec4 region(ivec2 p,ivec2 size){if(ViewCount==0)return ivec4(0,0,size);" \
    "for(int i=0;i<ViewCount;i++){ivec4 v=Views[i];" \
    "if(all(greaterThanEqual(p,v.xy))&&all(lessThan(p,v.xy+v.zw)))return v;}" \
    "return ivec4(0);}\n" \
    "vec3 bounded(sampler2D t,vec2 p,ivec4 v){vec2 size=vec2(textureSize(t,0));" \
    "p=clamp(p,vec2(v.xy)+.5,vec2(v.xy+v.zw)-.5);return texture(t,p/size).rgb;}\n"
#define BLOOM_DECODE \
    "uniform sampler2D Decode;\n" \
    "vec3 decode(uint v){return vec3(texelFetch(Decode,ivec2(int(v&31u),0),0).r," \
    "texelFetch(Decode,ivec2(int((v>>5)&31u),0),0).r," \
    "texelFetch(Decode,ivec2(int((v>>10)&31u),0),0).r);}\n"
#define BLOOM_ENCODE \
    "uniform usampler2D Encode;\n" \
    "vec3 encode(vec3 c){ivec3 v=ivec3(clamp(c*8192.0+.5,0.0,8192.0));" \
    "return vec3(texelFetch(Encode,ivec2(v.r%128,v.r/128),0).r," \
    "texelFetch(Encode,ivec2(v.g%128,v.g/128),0).r," \
    "texelFetch(Encode,ivec2(v.b%128,v.b/128),0).r)/255.0;}\n"

static const char gl_bloom_extract[] =
    "#version 330 core\n"
    "uniform usampler2D Scene; out vec4 Colour;\n"
    BLOOM_DECODE BLOOM_REGION
    "void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
    "ivec4 v=region(p,textureSize(Scene,0));"
    "if(v.z==0){Colour=vec4(0);return;}"
    "vec3 c=decode(texelFetch(Scene,p,0).r);float b=max(c.r,max(c.g,c.b));"
    "const float threshold=.3;"
    "float knee=clamp(b-threshold+.2,0.0,.4);knee=knee*knee/.8;"
    "Colour=vec4(c*max(b-threshold,knee)/max(b,.00001),1);}\n";

static const char gl_bloom_blur[] =
    "#version 330 core\n"
    "uniform sampler2D Image; uniform vec2 Step; out vec4 Colour;\n"
    BLOOM_REGION
    "void main(){vec2 p=gl_FragCoord.xy;ivec4 v=region(ivec2(p),textureSize(Image,0));"
    "if(v.z==0){Colour=vec4(0);return;}"
    "vec3 c=.227027027*bounded(Image,p,v);"
    "c+=.194594595*(bounded(Image,p+Step,v)+bounded(Image,p-Step,v));"
    "c+=.121621622*(bounded(Image,p+2.0*Step,v)+bounded(Image,p-2.0*Step,v));"
    "c+=.054054054*(bounded(Image,p+3.0*Step,v)+bounded(Image,p-3.0*Step,v));"
    "c+=.016216216*(bounded(Image,p+4.0*Step,v)+bounded(Image,p-4.0*Step,v));"
    "Colour=vec4(c,1);}\n";

#define BLOOM_COMPOSITE \
    "uniform usampler2D Source, Mask; uniform sampler2D Near, Wide;" \
    "uniform ivec2 Size;\n" \
    BLOOM_DECODE BLOOM_REGION \
    "vec3 glow(vec2 pos,ivec2 p){ivec4 v=region(p,textureSize(Source,0));" \
    "if(v.z==0||texelFetch(Mask,p,0).r!=0u)return vec3(0);" \
    "return .45*(.65*bounded(Near,pos,v)+.35*bounded(Wide,pos,v));}\n"

static const char gl_bloom_composite[] =
    "#version 330 core\n"
    "out vec4 Colour;\n" BLOOM_COMPOSITE
    "void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
    "Colour=vec4(min(vec3(1),decode(texelFetch(Source,p,0).r)+glow(gl_FragCoord.xy,p)),1);}\n";

static const char gl_bloom_output[] =
    "#version 330 core\n"
    "out vec4 Colour;\n" BLOOM_COMPOSITE BLOOM_ENCODE
    "void main(){ivec2 s=textureSize(Source,0);"
    "ivec2 p=clamp((ivec2(gl_FragCoord.xy)*2+1)*s/(Size*2),ivec2(0),s-1);"
    "uint v=texelFetch(Source,p,0).r;vec3 b=glow(gl_FragCoord.xy*vec2(s)/vec2(Size),p);"
    "if(all(equal(b,vec3(0)))){uvec3 rgb=(uvec3(v)>>uvec3(0,5,10))&31u;"
    "Colour=vec4(vec3((rgb<<3)|(rgb>>2))/255.0,1);}"
    "else Colour=vec4(encode(decode(v)+b),1);}\n";
#undef BLOOM_COMPOSITE
#undef BLOOM_ENCODE
#undef BLOOM_DECODE
#undef BLOOM_REGION
