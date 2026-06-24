#!/usr/bin/env bash
# Geometric-equivalence check between two slicer runs (for output-CHANGING migrations
# like Clipper2, where vertex-level g-code differs but geometry must be preserved).
# Compares extruded path length, extrusion-move count, layer count, and XY bbox.
# Usage: geom_equiv.sh <gcode_a> <gcode_b>
gstats(){ awk '
  /^G1 / {
    x=$0; e=""; nx=px; ny=py;
    if (match($0,/X-?[0-9.]+/)) nx=substr($0,RSTART+1,RLENGTH-1);
    if (match($0,/Y-?[0-9.]+/)) ny=substr($0,RSTART+1,RLENGTH-1);
    if (match($0,/Z-?[0-9.]+/)) { z=substr($0,RSTART+1,RLENGTH-1); if(z!=lz){nl++; lz=z} }
    if (match($0,/E-?[0-9.]+/)) { e=substr($0,RSTART+1,RLENGTH-1);
      if (e>pe) { dx=nx-px; dy=ny-py; len+=sqrt(dx*dx+dy*dy); ne++ }
      pe=e }
    px=nx; py=ny;
    if(nx<minx||minx==""){minx=nx} if(nx>maxx||maxx==""){maxx=nx}
    if(ny<miny||miny==""){miny=ny} if(ny>maxy||maxy==""){maxy=ny}
  }
  END{ printf "extruded_len=%.1f moves=%d layers=%d bbox=%.2f,%.2f,%.2f,%.2f\n", len,ne,nl,minx,miny,maxx,maxy }
' "$1"; }
echo "A: $(gstats "$1")"
echo "B: $(gstats "$2")"
