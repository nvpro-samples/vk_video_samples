inputFile="$1"       # for ex. ./Dstefan_cif.yuv.X264.264.yuv
inputResolution="$2" # for ex. 1920x1080

echo "Playing input file $inputFile with resolution of $inputResolution"
ffplay -v info -f rawvideo -pixel_format yuv420p -video_size $inputResolution $inputFile
