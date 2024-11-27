AM_CPPFLAGS = -I$(srcdir)/../cong_ctrl -I$(srcdir)/../util -I$(srcdir)/../decoder -I$(srcdir)/../display -I$(srcdir)/../input -I$(srcdir)/../encoder -I$(srcdir)/../net $(X264_CFLAGS) $(CXX11_FLAGS)
AM_CXXFLAGS = $(PICKY_CXXFLAGS) $(NODEBUG_CXXFLAGS)
AM_LDFLAGS = $(STATIC_BUILD_FLAG)
BASE_LDADD = ../cong_ctrl/libalfalfacong.a ../input/libalfalfainput.a ../decoder/libalfalfadecoder.a ../util/libalfalfautil.a $(X264_LIBS) $(JPEG_LIBS)

VP8PLAY_BUILD :=
if BUILDVP8PLAY
VP8PLAY_BUILD += salsify-receiver real-webcam display-jpeg receiver 
endif

bin_PROGRAMS = salsify-sender fake-webcam ivfcopy ivfcompare $(VP8PLAY_BUILD)

salsify_sender_SOURCES = salsify-sender.cc
salsify_sender_LDADD = ../net/libnet.a ../encoder/libalfalfaencoder.a $(BASE_LDADD)
salsify_sender_LDFLAGS = -pthread

salsify_receiver_SOURCES = salsify-receiver.cc
salsify_receiver_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
salsify_receiver_LDADD = ../display/libalfalfadisplay.a ../net/libnet.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
salsify_receiver_LDFLAGS = -pthread

fake_webcam_SOURCES = fake-webcam.cc
fake_webcam_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
fake_webcam_LDADD =  ../cong_ctrl/libalfalfacong.a ../net/libnet.a ../encoder/libalfalfaencoder.a ../display/libalfalfadisplay.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
fake_webcam_LDFLAGS = -pthread

real_webcam_SOURCES = real-webcam.cc
real_webcam_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
real_webcam_LDADD = ../display/libalfalfadisplay.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
real_webcam_LDFLAGS =

display_jpeg_SOURCES = display-jpeg.cc
display_jpeg_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
display_jpeg_LDADD = ../display/libalfalfadisplay.a ../net/libnet.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
display_jpeg_LDFLAGS =

ivfcopy_SOURCES = ivfcopy.cc
ivfcopy_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
ivfcopy_LDADD = ../util/libalfalfautil.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
ivfcopy_LDFLAGS =

ivfcompare_SOURCES = ivfcompare.cc
ivfcompare_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
ivfcompare_LDADD = ../util/libalfalfautil.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
ivfcompare_LDFLAGS =

receiver_SOURCES = receiver.cc
receiver_CPPFLAGS = $(AM_CPPFLAGS) $(GLU_CFLAGS) $(GLEW_CFLAGS) $(GLFW3_CFLAGS)
receiver_LDADD = ../display/libalfalfadisplay.a ../net/libnet.a $(BASE_LDADD) $(GLU_LIBS) $(GLEW_LIBS) $(GLFW3_LIBS)
receiver_LDFLAGS = -pthread
