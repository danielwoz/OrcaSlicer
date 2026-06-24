#!/usr/bin/env bash
# Travel + extrusion stats for a single g-code file.
# Reports:
#   travel_mm     : total length of non-extruding G1 XY moves (the thing the
#                   path-ordering refinement minimises)
#   extruded_mm   : total length of extruding G1 XY moves (must be invariant)
#   ext_moves     : count of extruding moves (must be invariant)
#   travel_moves  : count of travel moves
#   bbox          : XY bounding box (must be invariant)
# Usage: travel_stats.sh <gcode>
awk '
  function flush_seg() {}
  /^G1 / {
    nx=px; ny=py; has_e=0; e=pe;
    if (match($0,/X-?[0-9.]+/)) nx=substr($0,RSTART+1,RLENGTH-1);
    if (match($0,/Y-?[0-9.]+/)) ny=substr($0,RSTART+1,RLENGTH-1);
    if (match($0,/E-?[0-9.]+/)) { e=substr($0,RSTART+1,RLENGTH-1); has_e=1 }
    dx=nx-px; dy=ny-py; d=sqrt(dx*dx+dy*dy);
    if (has_e && e>pe) { extl+=d; ne++ }            # extruding move
    else if (d>0)      { travl+=d; nt++ }            # pure travel move
    if (has_e) pe=e;
    px=nx; py=ny;
    if(nx<minx||minx==""){minx=nx} if(nx>maxx||maxx==""){maxx=nx}
    if(ny<miny||miny==""){miny=ny} if(ny>maxy||maxy==""){maxy=ny}
  }
  /^G92 .*E0/ { pe=0 }
  END{ printf "travel_mm=%.1f extruded_mm=%.1f ext_moves=%d travel_moves=%d bbox=%.2f,%.2f,%.2f,%.2f\n",
       travl, extl, ne, nt, minx,miny,maxx,maxy }
' "$1"
