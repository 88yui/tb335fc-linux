#!/bin/sh
# camshot.sh [out.jpg] [nframes] — 慢速长曝多帧 + 全分辨率 ISP 拍照
OUT="$1"; [ -z "$OUT" ] && OUT=/root/photo.jpg
N="$2";   [ -z "$N" ]   && N=30
BASE=/root/shot_
rm -f ${BASE}*.raw
cd /root
rmmod camreg_isp 2>/dev/null; sleep 0.3
insmod /root/camreg_isp_top.ko devname=1a030000.cam1_legacy; sleep 0.3
MIN=$(awk '/camreg_isp/{print $1}' /proc/misc)
if [ -n "$MIN" ] && [ ! -c /dev/camreg ]; then mknod /dev/camreg c 10 "$MIN"; fi
chmod 666 /dev/camreg /proc/driver/camsensor 2>/dev/null
echo "camshot: n=$N out=$OUT (slow AE 4.7fps)"
CAMREC_AE_VTSMIN=16383 CAMREC_AE_VTSMAX=16383 CAMREC_AE_P995=470 \
  timeout 180 /root/camrec5 -n $N -p ${BASE} -v 16383 -e 15000 -g 64 -A -t 470 -k 4
echo "CAPTURE_RC=$?"
FRAMES=$(ls ${BASE}*.raw 2>/dev/null | tail -6)
echo "frames for ISP: $FRAMES"
if [ -z "$FRAMES" ]; then echo "NO FRAMES"; rmmod camreg_isp 2>/dev/null; exit 1; fi
/root/camisp -o /root/shot.ppm --scale 1 --sharpen 0.5 --tone v2 --awb-est v2 $FRAMES
echo "ISP_RC=$?"
convert /root/shot.ppm -quality 92 "$OUT" 2>/dev/null || /usr/bin/ffmpeg -y -loglevel error -i /root/shot.ppm "$OUT"
ls -l "$OUT"
rm -f ${BASE}*.raw /root/shot.ppm
rmmod camreg_isp 2>/dev/null
echo CAMSHOT_DONE
