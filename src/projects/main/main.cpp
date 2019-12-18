//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "main.h"

#include <iostream>
#include <regex>

#include <sys/utsname.h>

#include <srt/srt.h>
#include <srtp2/srtp.h>

#include <base/ovcrypto/ovcrypto.h>
#include <base/ovlibrary/daemon.h>
#include <base/ovlibrary/log_write.h>
#include <base/ovlibrary/stack_trace.h>
#include <base/ovlibrary/url.h>
#include <config/config_manager.h>

#include <media_router/media_router.h>
#include <orchestrator/orchestrator.h>
#include <providers/ovt/ovt_provider.h>
#include <providers/rtmp/rtmp_provider.h>
#include <publishers/ovt/ovt_publisher.h>
#include <publishers/segment/publishers.h>
#include <publishers/webrtc/webrtc_publisher.h>
#include <transcode/transcoder.h>

#include <monitoring/monitoring_server.h>
#include <web_console/web_console.h>

extern "C"
{
#include <libavutil/ffversion.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define CHECK_FAIL(x)                    \
	if ((x) == nullptr)                  \
	{                                    \
		srt_cleanup();                   \
                                         \
		if (parse_option.start_service)  \
		{                                \
			ov::Daemon::SetEvent(false); \
		}                                \
		return 1;                        \
	}

void SrtLogHandler(void *opaque, int level, const char *file, int line, const char *area, const char *message);

struct ParseOption
{
	// -c <config_path>
	ov::String config_path = "";

	// -s start with systemctl
	bool start_service = false;
};

ov::String GetFFmpegVersion(int version)
{
	return ov::String::FormatString("%d.%d.%d",
									AV_VERSION_MAJOR(version),
									AV_VERSION_MINOR(version),
									AV_VERSION_MICRO(version));
}

bool TryParseOption(int argc, char *argv[], ParseOption *parse_option)
{
	constexpr const char *opt_string = "hvt:c:d";

	while (true)
	{
		int name = getopt(argc, argv, opt_string);

		switch (name)
		{
			case -1:
				// end of arguments
				return true;

			case 'h':
				printf("Usage: %s [OPTION]...\n", argv[0]);
				printf("    -c <path>             Specify a path of config files\n");
				return false;

			case 'v':
				printf("OvenMediaEngine v%s\n", OME_VERSION);
				return false;

			case 'c':
				parse_option->config_path = optarg;
				break;

			case 'd':
				// Don't use this option manually
				parse_option->start_service = true;
				break;

			default:  // '?'
				// invalid argument
				return false;
		}
	}
}

int main(int argc, char *argv[])
{
	ParseOption parse_option;

	if (TryParseOption(argc, argv, &parse_option) == false)
	{
		return 1;
	}

	// Daemonize OME with start_service argument
	if (parse_option.start_service)
	{
		auto state = ov::Daemon::Initialize();

		if (state == ov::Daemon::State::PARENT_SUCCESS)
		{
			return 0;
		}
		else if (state == ov::Daemon::State::CHILD_SUCCESS)
		{
			// continue
		}
		else
		{
			logte("An error occurred while creating daemon");
			return 1;
		}
	}

	ov::StackTrace::InitializeStackTrace(OME_VERSION);

	ov::LogWrite::Initialize(parse_option.start_service);

	if (cfg::ConfigManager::Instance()->LoadConfigs(parse_option.config_path) == false)
	{
		logte("An error occurred while load config");
		return 1;
	}

	utsname uts{};
	::uname(&uts);

	logti("OvenMediaEngine v%s is started on [%s] (%s %s - %s, %s)", OME_VERSION, uts.nodename, uts.sysname, uts.machine, uts.release, uts.version);

	logti("With modules:");
	logti("  FFmpeg %s", FFMPEG_VERSION);
	logti("    Configuration: %s", avutil_configuration());
	logti("    libavformat: %s", GetFFmpegVersion(avformat_version()).CStr());
	logti("    libavcodec: %s", GetFFmpegVersion(avcodec_version()).CStr());
	logti("    libavfilter: %s", GetFFmpegVersion(avfilter_version()).CStr());
	logti("    libswresample: %s", GetFFmpegVersion(swresample_version()).CStr());
	logti("    libswscale: %s", GetFFmpegVersion(swscale_version()).CStr());
	logti("  SRT: %s", SRT_VERSION_STRING);
	logti("  OpenSSL: %s", OpenSSL_version(OPENSSL_VERSION));
	logti("    Configuration: %s", OpenSSL_version(OPENSSL_CFLAGS));

	logtd("Trying to initialize SRT...");
	srt_startup();
	srt_setloglevel(srt_logging::LogLevel::debug);
	srt_setloghandler(nullptr, SrtLogHandler);

	logtd("Trying to initialize OpenSSL...");
	ov::OpensslManager::InitializeOpenssl();

	logtd("Trying to initialize libsrtp...");
	int err = srtp_init();
	if (err != srtp_err_status_ok)
	{
		loge("SRTP", "Could not initialize SRTP (err: %d)", err);
		return 1;
	}

	std::shared_ptr<cfg::Server> server = cfg::ConfigManager::Instance()->GetServer();
	std::vector<cfg::Host> hosts = server->GetHostList();

	std::shared_ptr<MediaRouter> router;
	std::shared_ptr<Transcoder> transcoder;
	std::vector<std::shared_ptr<WebConsoleServer>> web_console_servers;

	std::vector<info::Host> host_info_list;

	auto orchestrator = Orchestrator::GetInstance();

	// Create info::Host
	for (const auto &host : hosts)
	{
		logti("Trying to create host info [%s]...", host.GetName().CStr());
		host_info_list.emplace_back(host);
	}

	for (auto &host_info : host_info_list)
	{
		auto host_name = host_info.GetName();

		logtd("Trying to create modules for host [%s]", host_name.CStr());

		// TODO(dimiden): Support virtual host
		orchestrator->PrepareOriginMap(host_info.GetOrigins());

		//////////////////////////////
		// Host Level Modules
		//TODO(Getroot): Support Virtual Host Function. This code assumes that there is only one Host.
		//////////////////////////////

		logti("Trying to create MediaRouter for host [%s]...", host_name.CStr());
		router = MediaRouter::Create();
		CHECK_FAIL(router);

		logti("Trying to create Transcoder for host [%s]...", host_name.CStr());
		transcoder = Transcoder::Create(router);
		CHECK_FAIL(transcoder);

		logti("Trying to create RTMP Provider [%s]...", host_name.CStr());
		auto rtmp_provider = RtmpProvider::Create(host_info, router);
		CHECK_FAIL(rtmp_provider);

		logti("Trying to create OVT Provider [%s]...", host_name.CStr());
		auto ovt_provider = pvd::OvtProvider::Create(host_info, router);
		CHECK_FAIL(ovt_provider);

		logti("Trying to create WebRTC Publisher [%s]...", host_name.CStr());
		auto webrtc_publisher = WebRtcPublisher::Create(host_info, router);
		CHECK_FAIL(webrtc_publisher);

		logti("Trying to create OVT Publisher [%s]...", host_name.CStr());
		auto ovt_publisher = OvtPublisher::Create(host_info, router);
		CHECK_FAIL(ovt_publisher);

		//--------------------------------------------------------------------
		// Register modules to Orchestrator
		//--------------------------------------------------------------------
		// Register providers
		orchestrator->RegisterModule(rtmp_provider);
		orchestrator->RegisterModule(ovt_provider);
		// Register media router
		orchestrator->RegisterModule(router);
		// Register transcoder
		orchestrator->RegisterModule(transcoder);
		// Register publishers
		orchestrator->RegisterModule(webrtc_publisher);
		orchestrator->RegisterModule(ovt_publisher);

		// Create applications that defined by the configuration
		for (auto app_cfg : host_info.GetApplicationList())
		{
			orchestrator->CreateApplication(app_cfg);
		}

		/* For Edge Test */
#if 0
		while(true)
		{
			sleep(1);
			if(ovt_provider->PullStream("ovt://192.168.0.199:9000/app/stream_o"))
			{
				break;
			}
		}
#endif
	}

	if (parse_option.start_service)
	{
		ov::Daemon::SetEvent();
	}

	while (true)
	{
		sleep(1);
	}

	logtd("Trying to uninitialize OpenSSL...");
	ov::OpensslManager::ReleaseOpenSSL();

	srt_cleanup();

	logti("OvenMediaEngine will be terminated");

	return 0;
}

void SrtLogHandler(void *opaque, int level, const char *file, int line, const char *area, const char *message)
{
	// SRT log format:
	// HH:mm:ss.ssssss/SRT:xxxx:xxxxxx.N: xxx.c: .................
	// 13:20:15.618019.N: SRT.c: PASSING request from: 192.168.0.212:63308 to agent:397692317
	// 13:20:15.618019.!!FATAL!!: SRT.c: PASSING request from: 192.168.0.212:63308 to agent:397692317
	// 13:20:15.618019.!W: SRT.c: PASSING request from: 192.168.0.212:63308 to agent:397692317
	// 13:20:15.618019.*E: SRT.c: PASSING request from: 192.168.0.212:63308 to agent:397692317
	// 20:41:11.929158/ome_origin*E: SRT.d: SND-DROPPED 1 packets - lost delaying for 1021m
	// 13:20:15.618019/SRT:RcvQ:worker.N: SRT.c: PASSING request from: 192.168.0.212:63308 to agent:397692317

	std::smatch matches;
	std::string m = message;
	ov::String mess;

	if (std::regex_search(m, matches, std::regex("^([0-9]{2}:[0-9]{2}:[0-9]{2}.[0-9]{6})([/a-zA-Z:.!*_]+) ([/a-zA-Z:.!]+ )?(.+)")))
	{
		mess = std::string(matches[4]).c_str();
	}
	else
	{
		// Unknown pattern
		mess = message;
	}

	const char *SRT_LOG_TAG = "SRT";

	switch (level)
	{
		case srt_logging::LogLevel::debug:
			ov_log_internal(OVLogLevelDebug, SRT_LOG_TAG, file, line, area, "%s", mess.CStr());
			break;

		case srt_logging::LogLevel::note:
			ov_log_internal(OVLogLevelInformation, SRT_LOG_TAG, file, line, area, "%s", mess.CStr());
			break;

		case srt_logging::LogLevel::warning:
			ov_log_internal(OVLogLevelWarning, SRT_LOG_TAG, file, line, area, "%s", mess.CStr());
			break;

		case srt_logging::LogLevel::error:
			ov_log_internal(OVLogLevelError, SRT_LOG_TAG, file, line, area, "%s", mess.CStr());
			break;

		case srt_logging::LogLevel::fatal:
			ov_log_internal(OVLogLevelCritical, SRT_LOG_TAG, file, line, area, "%s", mess.CStr());
			break;

		default:
			ov_log_internal(OVLogLevelError, SRT_LOG_TAG, file, line, area, "(Unknown level: %d) %s", level, mess.CStr());
			break;
	}
}
