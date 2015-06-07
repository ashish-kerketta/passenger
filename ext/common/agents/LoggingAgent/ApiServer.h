/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2013-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_LOGGING_AGENT_API_SERVER_H_
#define _PASSENGER_LOGGING_AGENT_API_SERVER_H_

#include <sstream>
#include <string>

#include <agents/LoggingAgent/LoggingServer.h>
#include <agents/ApiServerUtils.h>
#include <ApplicationPool2/ApiKey.h>
#include <ServerKit/HttpServer.h>
#include <DataStructures/LString.h>
#include <Exceptions.h>
#include <StaticString.h>
#include <Utils/StrIntUtils.h>
#include <Utils/modp_b64.h>
#include <Utils/json.h>
#include <Utils/BufferedIO.h>
#include <Utils/MessageIO.h>

namespace Passenger {
namespace LoggingAgent {

using namespace std;


class Request: public ServerKit::BaseHttpRequest {
public:
	string body;
	Json::Value jsonBody;

	DEFINE_SERVER_KIT_BASE_HTTP_REQUEST_FOOTER(Request);
};

class ApiServer: public ServerKit::HttpServer<ApiServer, ServerKit::HttpClient<Request> > {
private:
	typedef ServerKit::HttpServer<ApiServer, ServerKit::HttpClient<Request> > ParentClass;
	typedef ServerKit::HttpClient<Request> Client;
	typedef ServerKit::HeaderTable HeaderTable;

	void route(Client *client, Request *req, const StaticString &path) {
		if (path == P_STATIC_STRING("/ping.json")) {
			apiServerProcessPing(this, client, req);
		} else if (path == P_STATIC_STRING("/version.json")) {
			apiServerProcessVersion(this, client, req);
		} else if (path == P_STATIC_STRING("/shutdown.json")) {
			apiServerProcessShutdown(this, client, req);
		} else if (path == P_STATIC_STRING("/backtraces.txt")) {
			apiServerProcessBacktraces(this, client, req);
		} else if (path == P_STATIC_STRING("/config.json")) {
			processConfig(client, req);
		} else if (path == P_STATIC_STRING("/reinherit_logs.json")) {
			apiServerProcessReinheritLogs(this, client, req,
				instanceDir, fdPassingPassword);
		} else if (path == P_STATIC_STRING("/reopen_logs.json")) {
			apiServerProcessReopenLogs(this, client, req);
		} else if (path == P_STATIC_STRING("/status.txt")) {
			processStatusTxt(client, req);
		} else {
			apiServerRespondWith404(this, client, req);
		}
	}

	void processConfig(Client *client, Request *req) {
		if (req->method == HTTP_GET) {
			if (!authorizeStateInspectionOperation(this, client, req)) {
				apiServerRespondWith401(this, client, req);
			}

			HeaderTable headers;
			Json::Value doc;
			string logFile = getLogFile();
			string fileDescriptorLogFile = getFileDescriptorLogFile();

			headers.insert(req->pool, "Content-Type", "application/json");
			doc["log_level"] = getLogLevel();
			if (!logFile.empty()) {
				doc["log_file"] = logFile;
			}
			if (!fileDescriptorLogFile.empty()) {
				doc["file_descriptor_log_file"] = fileDescriptorLogFile;
			}

			writeSimpleResponse(client, 200, &headers, doc.toStyledString());
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else if (req->method == HTTP_PUT) {
			if (!authorizeAdminOperation(this, client, req)) {
				apiServerRespondWith401(this, client, req);
			} else if (!req->hasBody()) {
				endAsBadRequest(&client, &req, "Body required");
			}
			// Continue in processConfigBody()
		} else {
			apiServerRespondWith405(this, client, req);
		}
	}

	void processConfigBody(Client *client, Request *req) {
		HeaderTable headers;
		Json::Value &json = req->jsonBody;

		headers.insert(req->pool, "Content-Type", "application/json");

		if (json.isMember("log_level")) {
			setLogLevel(json["log_level"].asInt());
		}
		if (json.isMember("log_file")) {
			string logFile = json["log_file"].asString();
			try {
				logFile = absolutizePath(logFile);
			} catch (const SystemException &e) {
				unsigned int bufsize = 1024;
				char *message = (char *) psg_pnalloc(req->pool, bufsize);
				snprintf(message, bufsize, "{ \"status\": \"error\", "
					"\"message\": \"Cannot absolutize log file filename: %s\" }",
					e.what());
				writeSimpleResponse(client, 500, &headers, message);
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			int e;
			if (!setLogFile(logFile, &e)) {
				unsigned int bufsize = 1024;
				char *message = (char *) psg_pnalloc(req->pool, bufsize);
				snprintf(message, bufsize, "{ \"status\": \"error\", "
					"\"message\": \"Cannot open log file: %s (errno=%d)\" }",
					strerror(e), e);
				writeSimpleResponse(client, 500, &headers, message);
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}
			P_NOTICE("Log file opened.");
		}

		writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }\n");
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void processStatusTxt(Client *client, Request *req) {
		if (req->method != HTTP_GET) {
			apiServerRespondWith405(this, client, req);
		} else if (authorizeStateInspectionOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "text/plain");

			stringstream stream;
			loggingServer->dump(stream);
			writeSimpleResponse(client, 200, &headers, stream.str());
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			apiServerRespondWith401(this, client, req);
		}
	}

protected:
	virtual void onRequestBegin(Client *client, Request *req) {
		const StaticString path(req->path.start->data, req->path.size);

		P_INFO("API request: " << http_method_str(req->method) <<
			" " << StaticString(req->path.start->data, req->path.size));

		try {
			route(client, req, path);
		} catch (const oxt::tracable_exception &e) {
			SKC_ERROR(client, "Exception: " << e.what() << "\n" << e.backtrace());
			if (!req->ended()) {
				req->wantKeepAlive = false;
				endRequest(&client, &req);
			}
		}
	}

	virtual ServerKit::Channel::Result onRequestBody(Client *client, Request *req,
		const MemoryKit::mbuf &buffer, int errcode)
	{
		if (buffer.size() > 0) {
			// Data
			req->body.append(buffer.start, buffer.size());
		} else if (errcode == 0) {
			// EOF
			Json::Reader reader;
			if (reader.parse(req->body, req->jsonBody)) {
				try {
					processConfigBody(client, req);
				} catch (const oxt::tracable_exception &e) {
					SKC_ERROR(client, "Exception: " << e.what() << "\n" << e.backtrace());
					if (!req->ended()) {
						req->wantKeepAlive = false;
						endRequest(&client, &req);
					}
				}
			} else {
				apiServerRespondWith422(this, client, req, reader.getFormattedErrorMessages());
			}
		} else {
			// Error
			disconnect(&client);
		}
		return ServerKit::Channel::Result(buffer.size(), false);
	}

	virtual void deinitializeRequest(Client *client, Request *req) {
		req->body.clear();
		if (!req->jsonBody.isNull()) {
			req->jsonBody = Json::Value();
		}
		ParentClass::deinitializeRequest(client, req);
	}

public:
	LoggingServer *loggingServer;
	ApiAccountDatabase *apiAccountDatabase;
	string instanceDir;
	string fdPassingPassword;
	EventFd *exitEvent;

	ApiServer(ServerKit::Context *context)
		: ParentClass(context),
		  loggingServer(NULL),
		  apiAccountDatabase(NULL),
		  exitEvent(NULL)
		{ }

	virtual StaticString getServerName() const {
		return P_STATIC_STRING("LoggerApiServer");
	}

	virtual unsigned int getClientName(const Client *client, char *buf, size_t size) const {
		return ParentClass::getClientName(client, buf, size);
	}

	bool authorizeByUid(uid_t uid) const {
		return uid == 0 || uid == geteuid();
	}

	bool authorizeByApiKey(const ApplicationPool2::ApiKey &apiKey) const {
		return apiKey.isSuper();
	}
};


} // namespace LoggingAgent
} // namespace Passenger

#endif /* _PASSENGER_LOGGING_AGENT_API_SERVER_H_ */
