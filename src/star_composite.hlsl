// Adds a temporally accumulated star layer, sky only, to RR's output.
// The game draws stars as sub-pixel points: in RR's input a given star's
// brightness varies ~10x from frame to frame, depending on whether that frame's
// jittered sample hits it. RR discards them; drawing the raw per-frame excess
// shimmers. So the per-frame excess is blended into a history buffer at output
// resolution (exponential average), reprojected with the tagged motion vectors
// (Streamline convention: previous = current + mv), and the history is what gets
// added. The blend weight goes from `alpha_still` (long average, no twinkle) to
// `alpha_moving` (fast response, little smearing) as sky motion reaches 8
// output pixels per frame.
// One thread per output pixel; each thread writes only its own pixel.
struct Constants {
  uint2 in_size; uint2 out_size; float2 scale; float2 mvec_scale;
  float alpha_still; float gain; uint reset; float alpha_moving;
};
ConstantBuffer<Constants> c : register(b0);
Texture2D<float4> color : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float2> motion : register(t2);
Texture2D<float4> history : register(t3);
RWTexture2D<float4> output : register(u0);
RWTexture2D<float4> next_history : register(u1);
SamplerState linear_clamp : register(s0);
float Luma(float3 v) { return dot(v, float3(.2126, .7152, .0722)); }
// Excess brightness of a sky star at input pixel p, 0 if none. Detection matches
// tools/snapshot_analyze.cpp: a 3x3 local maximum compared with the ring at
// distance 2..4, with a soft knee from 1.5x (weight 0) to 2.5x (weight 1).
float3 Excess(int2 p) {
  const float3 center = color.Load(int3(p, 0)).rgb;
  const float v = Luma(center);
  if (!(v > 0) || any(!isfinite(center))) return 0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
      if ((x || y) && Luma(color.Load(int3(p + int2(x, y), 0)).rgb) > v) return 0;
  float3 ring = 0;
  [unroll] for (int ry = -4; ry <= 4; ++ry)
    [unroll] for (int rx = -4; rx <= 4; ++rx)
      if (max(abs(rx), abs(ry)) >= 2) ring += color.Load(int3(p + int2(rx, ry), 0)).rgb;
  ring /= 72;
  const float r = max(Luma(ring), 1e-6);
  if (!(v - r > 1e-4) || any(!isfinite(ring))) return 0;
  return max(center - ring, 0) * saturate(v / r - 1.5);
}
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= c.out_size)) return;
  const int2 p = int2((float2(id.xy) + .5) * c.scale);
  const bool inside = all(p >= 4) && all(p + 4 < int2(c.in_size));
  // Trees, terrain and buildings (depth > 0) clear the layer, so stars never show in front of them.
  if (!inside || !(depth.Load(int3(p, 0)) <= 0)) { next_history[id.xy] = 0; return; }
  float2 mv = motion.Load(int3(p, 0)) * c.mvec_scale;
  if (any(!isfinite(mv))) mv = 0;
  float3 previous = 0;
  if (!c.reset) {
    const float2 uv = (float2(id.xy) + .5) / float2(c.out_size) + mv;
    if (all(uv >= 0) && all(uv <= 1)) previous = history.SampleLevel(linear_clamp, uv, 0).rgb;
  }
  const float alpha = lerp(c.alpha_still, c.alpha_moving, saturate(length(mv * float2(c.out_size)) / 8));
  const float3 layer = lerp(previous, Excess(p), alpha);
  next_history[id.xy] = float4(layer, 0);
  // Most sky pixels have no star; adding 0 would only re-read and re-write RR's output.
  if (any(layer != 0)) {
    float4 o = output[id.xy];
    o.rgb += layer * c.gain;
    output[id.xy] = o;
  }
}
