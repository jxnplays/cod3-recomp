# FFmpeg's headers, fetched by scripts/get_ffmpeg_headers.ps1.
#
# Not committed on purpose. The build needs libavcodec/avcodec.h and a few
# libavutil headers to compile xma.cpp, but the FFmpeg builds that provide them
# are GPL, and committing GPL headers into a repository whose own work is MIT
# would muddy the licensing of the whole project for no benefit. The fetch is
# scripted instead, so it is one command rather than a vendored copy.
thirdparty/ffmpeg/
