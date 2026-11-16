# GpuPassDetail summary: groups the "GPU pass label" lines of ONE report into passes.
# n=$(grep -n "GPU pass detail" bmvr_log.txt | tail -2 | head -1 | cut -d: -f1); m=$(grep -n "GPU pass detail" bmvr_log.txt | tail -1 | cut -d: -f1)
# sed -n "$n,$((m-1))p" bmvr_log.txt | grep "GPU pass label" | awk -f tools/headless/gpu-pass-summary.awk
# Buckets: RT0 format (f111 = G-buffer, f35 = light buffer, 4096 = shadow depth, f113 = bloom chain) and material group.
{
  split($0, p, " \\| "); rt=p[2]; mat=p[3];
  match($0, /L +[0-9.]+ R +[0-9.]+ O +[0-9.]+/); f=substr($0,RSTART,RLENGTH); split(f,v," +");
  l=v[2]; r=v[4]; o=v[6];
  c="main pass: other";
  if (mat ~ /after Present/) c="after Present (idle + present)";
  else if (mat ~ /^mod eyes/) c="mod: OpenXR publish copy";
  else if (mat ~ /^mod /) c="mod: mirror/overlays/toast";
  else if (mat ~ /\(refract\)/) c="refract copy";
  else if (mat ~ /^copy/) c="eye copy (post, StretchRect)";
  else if (mat ~ /^msaa/) c="msaa retire/begin";
  else if (rt ~ /^4096x4096/) c="shadow depth";
  else if (rt ~ / f111 /) c="G-buffer (depth/normals 1x)";
  else if (rt ~ / f35 /) c="deferred lights (1x light buffer)";
  else if (mat ~ /^dev\// || rt ~ /f113/) c="post (bloom/tonemap/postprocess)";
  else if (mat ~ /^particle\/|^effects\/|sprites/) c="particles/effects";
  else if (mat ~ /gbxog0/) c="Xen fog global";
  else if (mat ~ /gbxog\*/) c="Xen fog volumes";
  else if (mat ~ /water/) c="water";
  else if (mat ~ /skybox/) c="skybox (+3D sky props)";
  else if (mat ~ /^maps\//) c="main pass: world brushes";
  else if (mat ~ /^models\//) c="main pass: models";
  L[c]+=l; R[c]+=r; O[c]+=o; tl+=l; tr+=r; to+=o
}
END { for (c in L) printf "%6.2f  L %5.2f R %5.2f O %5.2f  %s\n", L[c]+R[c]+O[c], L[c], R[c], O[c], c | "sort -rn"; close("sort -rn"); printf "%6.2f  L %5.2f R %5.2f O %5.2f  TOTAL\n", tl+tr+to, tl, tr, to }
