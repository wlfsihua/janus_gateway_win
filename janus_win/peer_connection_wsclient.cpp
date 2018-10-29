#include "peer_connection_wsclient.h"
#include "defaults.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/nethelpers.h"
#include "rtc_base/stringutils.h"

#include "rtc_base/json.h"

#ifdef WIN32
#include "rtc_base/win32socketserver.h"
#endif

using rtc::sprintfn;

namespace {

	// This is our magical hangup signal.
	const char kByeMessage[] = "BYE";
	// Delay between server connection retries, in milliseconds
	const int kReconnectDelay = 2000;

	rtc::AsyncSocket* CreateClientSocket(int family) {
#ifdef WIN32
		rtc::Win32Socket* sock = new rtc::Win32Socket();
		sock->CreateT(family, SOCK_STREAM);
		return sock;
#elif defined(WEBRTC_POSIX)
		rtc::Thread* thread = rtc::Thread::Current();
		RTC_DCHECK(thread != NULL);
		return thread->socketserver()->CreateAsyncSocket(family, SOCK_STREAM);
#else
#error Platform not supported.
#endif
	}

}  // namespace

PeerConnectionWsClient::PeerConnectionWsClient()
	: callback_(NULL), resolver_(NULL), state_(NOT_CONNECTED), my_id_(-1) {}

PeerConnectionWsClient::~PeerConnectionWsClient() {}

void PeerConnectionWsClient::InitSocketSignals() {
	RTC_DCHECK(control_socket_.get() != NULL);
	RTC_DCHECK(hanging_get_.get() != NULL);
	control_socket_->SignalCloseEvent.connect(this,
		&PeerConnectionWsClient::OnClose);
	hanging_get_->SignalCloseEvent.connect(this, &PeerConnectionWsClient::OnClose);
	control_socket_->SignalConnectEvent.connect(this,
		&PeerConnectionWsClient::OnConnect);
	hanging_get_->SignalConnectEvent.connect(
		this, &PeerConnectionWsClient::OnHangingGetConnect);
	control_socket_->SignalReadEvent.connect(this, &PeerConnectionWsClient::OnRead);
	hanging_get_->SignalReadEvent.connect(
		this, &PeerConnectionWsClient::OnHangingGetRead);
}

int PeerConnectionWsClient::id() const {
	return my_id_;
}

bool PeerConnectionWsClient::is_connected() const {
	return my_id_ != -1;
}

const Peers& PeerConnectionWsClient::peers() const {
	return peers_;
}

void PeerConnectionWsClient::RegisterObserver(
	PeerConnectionWsClientObserver* callback) {
	RTC_DCHECK(!callback_);
	callback_ = callback;
}

//uWs init and connnect
void PeerConnectionWsClient::Connect(const std::string& server,
	const std::string& client_id) {
	//error handler
	h.onError([](void *user) {
		PeerConnectionWsClient* pws = (PeerConnectionWsClient*)user;
		if (pws->state_ != NOT_CONNECTED) {
			RTC_LOG(WARNING)
				<< "The client must not be connected before you can call Connect()";
			pws->callback_->OnServerConnectionFailure();
		}
	});
	//connection handler
	h.onConnection([](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req) {
		//get user data
		long that_ptr =(long)ws->getUserData();
		PeerConnectionWsClient* pws = (PeerConnectionWsClient*)(ws->getUserData());
		if (pws->state_ == NOT_CONNECTED) {
			RTC_LOG(WARNING) << "Client established a remote connection over non-SSL";
			pws->state_ == CONNECTED;

	
			//create session
			Json::StyledWriter writer;
			Json::Value jmessage;

			jmessage["janus"] = "create";
			jmessage["transaction"] = pws->RandomString(12);
			std::string json_str = writer.write(jmessage).c_str();
			ws->send(writer.write(jmessage).c_str(), uWS::TEXT);
		}
		switch ((long)ws->getUserData()) {
		case 1:
			RTC_LOG(WARNING) << "Client established a remote connection over non-SSL";
			std::cout << "Client established a remote connection over non-SSL" << std::endl;
			ws->close(1000);
			break;
		default:
			std::cout << "FAILURE: " << ws->getUserData() << " should not connect!" << std::endl;
			exit(-1);
		}
	});

	h.onDisconnection([](uWS::WebSocket<uWS::CLIENT> *ws, int code, char *message, size_t length) {
		RTC_LOG(WARNING) << "Client got disconnected";
		std::cout << "Client got disconnected with data: " << ws->getUserData() << ", code: " << code << ", message: <" << std::string(message, length) << ">" << std::endl;
	});

	h.onMessage([](uWS::WebSocket<uWS::CLIENT> *ws, char *message, size_t length, uWS::OpCode opCode) {
		PeerConnectionWsClient* pws = (PeerConnectionWsClient*)(ws->getUserData());
		pws->handleMessages(message,length);
	});


	std::map<std::string, std::string> protocol_map;
	protocol_map.insert(std::pair<std::string, std::string>(std::string("Sec-WebSocket-Protocol"), std::string("janus-protocol")));
	long this_ptr = (long)this;
	h.connect("ws://39.106.100.180:8188", (void*)this, protocol_map);
	h.run();
	std::cout << "Falling through testConnections" << std::endl;
}

void PeerConnectionWsClient::OnResolveResult(
	rtc::AsyncResolverInterface* resolver) {
	if (resolver_->GetError() != 0) {
		callback_->OnServerConnectionFailure();
		resolver_->Destroy(false);
		resolver_ = NULL;
		state_ = NOT_CONNECTED;
	}
	else {
		server_address_ = resolver_->address();
		DoConnect();
	}
}

void PeerConnectionWsClient::DoConnect() {
	control_socket_.reset(CreateClientSocket(server_address_.ipaddr().family()));
	hanging_get_.reset(CreateClientSocket(server_address_.ipaddr().family()));
	InitSocketSignals();
	char buffer[1024];
	sprintfn(buffer, sizeof(buffer), "GET /sign_in?%s HTTP/1.0\r\n\r\n",
		client_name_.c_str());
	onconnect_data_ = buffer;

	bool ret = ConnectControlSocket();
	if (ret)
		state_ = SIGNING_IN;
	if (!ret) {
		callback_->OnServerConnectionFailure();
	}
}

bool PeerConnectionWsClient::SendToPeer(int peer_id, const std::string& message) {
	if (state_ != CONNECTED)
		return false;

	RTC_DCHECK(is_connected());
	RTC_DCHECK(control_socket_->GetState() == rtc::Socket::CS_CLOSED);
	if (!is_connected() || peer_id == -1)
		return false;

	char headers[1024];
	sprintfn(headers, sizeof(headers),
		"POST /message?peer_id=%i&to=%i HTTP/1.0\r\n"
		"Content-Length: %i\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n",
		my_id_, peer_id, message.length());
	onconnect_data_ = headers;
	onconnect_data_ += message;
	return ConnectControlSocket();
}

bool PeerConnectionWsClient::SendHangUp(int peer_id) {
	return SendToPeer(peer_id, kByeMessage);
}

bool PeerConnectionWsClient::IsSendingMessage() {
	return state_ == CONNECTED &&
		control_socket_->GetState() != rtc::Socket::CS_CLOSED;
}

bool PeerConnectionWsClient::SignOut() {
	if (state_ == NOT_CONNECTED || state_ == SIGNING_OUT)
		return true;

	if (hanging_get_->GetState() != rtc::Socket::CS_CLOSED)
		hanging_get_->Close();

	if (control_socket_->GetState() == rtc::Socket::CS_CLOSED) {
		state_ = SIGNING_OUT;

		if (my_id_ != -1) {
			char buffer[1024];
			sprintfn(buffer, sizeof(buffer),
				"GET /sign_out?peer_id=%i HTTP/1.0\r\n\r\n", my_id_);
			onconnect_data_ = buffer;
			return ConnectControlSocket();
		}
		else {
			// Can occur if the app is closed before we finish connecting.
			return true;
		}
	}
	else {
		state_ = SIGNING_OUT_WAITING;
	}

	return true;
}

void PeerConnectionWsClient::Close() {
	control_socket_->Close();
	hanging_get_->Close();
	onconnect_data_.clear();
	peers_.clear();
	if (resolver_ != NULL) {
		resolver_->Destroy(false);
		resolver_ = NULL;
	}
	my_id_ = -1;
	state_ = NOT_CONNECTED;
}

bool PeerConnectionWsClient::ConnectControlSocket() {
	RTC_DCHECK(control_socket_->GetState() == rtc::Socket::CS_CLOSED);
	int err = control_socket_->Connect(server_address_);
	if (err == SOCKET_ERROR) {
		Close();
		return false;
	}
	return true;
}

void PeerConnectionWsClient::OnConnect(rtc::AsyncSocket* socket) {
	RTC_DCHECK(!onconnect_data_.empty());
	size_t sent = socket->Send(onconnect_data_.c_str(), onconnect_data_.length());
	RTC_DCHECK(sent == onconnect_data_.length());
	onconnect_data_.clear();
}

void PeerConnectionWsClient::OnHangingGetConnect(rtc::AsyncSocket* socket) {
	char buffer[1024];
	sprintfn(buffer, sizeof(buffer), "GET /wait?peer_id=%i HTTP/1.0\r\n\r\n",
		my_id_);
	int len = static_cast<int>(strlen(buffer));
	int sent = socket->Send(buffer, len);
	RTC_DCHECK(sent == len);
}

void PeerConnectionWsClient::OnMessageFromPeer(int peer_id,
	const std::string& message) {
	if (message.length() == (sizeof(kByeMessage) - 1) &&
		message.compare(kByeMessage) == 0) {
		callback_->OnPeerDisconnected(peer_id);
	}
	else {
		callback_->OnMessageFromPeer(peer_id, message);
	}
}

bool PeerConnectionWsClient::GetHeaderValue(const std::string& data,
	size_t eoh,
	const char* header_pattern,
	size_t* value) {
	RTC_DCHECK(value != NULL);
	size_t found = data.find(header_pattern);
	if (found != std::string::npos && found < eoh) {
		*value = atoi(&data[found + strlen(header_pattern)]);
		return true;
	}
	return false;
}

bool PeerConnectionWsClient::GetHeaderValue(const std::string& data,
	size_t eoh,
	const char* header_pattern,
	std::string* value) {
	RTC_DCHECK(value != NULL);
	size_t found = data.find(header_pattern);
	if (found != std::string::npos && found < eoh) {
		size_t begin = found + strlen(header_pattern);
		size_t end = data.find("\r\n", begin);
		if (end == std::string::npos)
			end = eoh;
		value->assign(data.substr(begin, end - begin));
		return true;
	}
	return false;
}

bool PeerConnectionWsClient::ReadIntoBuffer(rtc::AsyncSocket* socket,
	std::string* data,
	size_t* content_length) {
	char buffer[0xffff];
	do {
		int bytes = socket->Recv(buffer, sizeof(buffer), nullptr);
		if (bytes <= 0)
			break;
		data->append(buffer, bytes);
	} while (true);

	bool ret = false;
	size_t i = data->find("\r\n\r\n");
	if (i != std::string::npos) {
		RTC_LOG(INFO) << "Headers received";
		if (GetHeaderValue(*data, i, "\r\nContent-Length: ", content_length)) {
			size_t total_response_size = (i + 4) + *content_length;
			if (data->length() >= total_response_size) {
				ret = true;
				std::string should_close;
				const char kConnection[] = "\r\nConnection: ";
				if (GetHeaderValue(*data, i, kConnection, &should_close) &&
					should_close.compare("close") == 0) {
					socket->Close();
					// Since we closed the socket, there was no notification delivered
					// to us.  Compensate by letting ourselves know.
					OnClose(socket, 0);
				}
			}
			else {
				// We haven't received everything.  Just continue to accept data.
			}
		}
		else {
			RTC_LOG(LS_ERROR) << "No content length field specified by the server.";
		}
	}
	return ret;
}

void PeerConnectionWsClient::OnRead(rtc::AsyncSocket* socket) {
	size_t content_length = 0;
	if (ReadIntoBuffer(socket, &control_data_, &content_length)) {
		size_t peer_id = 0, eoh = 0;
		bool ok =
			ParseServerResponse(control_data_, content_length, &peer_id, &eoh);
		if (ok) {
			if (my_id_ == -1) {
				// First response.  Let's store our server assigned ID.
				RTC_DCHECK(state_ == SIGNING_IN);
				my_id_ = static_cast<int>(peer_id);
				RTC_DCHECK(my_id_ != -1);

				// The body of the response will be a list of already connected peers.
				if (content_length) {
					size_t pos = eoh + 4;
					while (pos < control_data_.size()) {
						size_t eol = control_data_.find('\n', pos);
						if (eol == std::string::npos)
							break;
						int id = 0;
						std::string name;
						bool connected;
						if (ParseEntry(control_data_.substr(pos, eol - pos), &name, &id,
							&connected) &&
							id != my_id_) {
							peers_[id] = name;
							callback_->OnPeerConnected(id, name);
						}
						pos = eol + 1;
					}
				}
				RTC_DCHECK(is_connected());
				callback_->OnSignedIn();
			}
			else if (state_ == SIGNING_OUT) {
				Close();
				callback_->OnDisconnected();
			}
			else if (state_ == SIGNING_OUT_WAITING) {
				SignOut();
			}
		}

		control_data_.clear();

		if (state_ == SIGNING_IN) {
			RTC_DCHECK(hanging_get_->GetState() == rtc::Socket::CS_CLOSED);
			state_ = CONNECTED;
			hanging_get_->Connect(server_address_);
		}
	}
}

void PeerConnectionWsClient::OnHangingGetRead(rtc::AsyncSocket* socket) {
	RTC_LOG(INFO) << __FUNCTION__;
	size_t content_length = 0;
	if (ReadIntoBuffer(socket, &notification_data_, &content_length)) {
		size_t peer_id = 0, eoh = 0;
		bool ok =
			ParseServerResponse(notification_data_, content_length, &peer_id, &eoh);

		if (ok) {
			// Store the position where the body begins.
			size_t pos = eoh + 4;

			if (my_id_ == static_cast<int>(peer_id)) {
				// A notification about a new member or a member that just
				// disconnected.
				int id = 0;
				std::string name;
				bool connected = false;
				if (ParseEntry(notification_data_.substr(pos), &name, &id,
					&connected)) {
					if (connected) {
						peers_[id] = name;
						callback_->OnPeerConnected(id, name);
					}
					else {
						peers_.erase(id);
						callback_->OnPeerDisconnected(id);
					}
				}
			}
			else {
				OnMessageFromPeer(static_cast<int>(peer_id),
					notification_data_.substr(pos));
			}
		}

		notification_data_.clear();
	}

	if (hanging_get_->GetState() == rtc::Socket::CS_CLOSED &&
		state_ == CONNECTED) {
		hanging_get_->Connect(server_address_);
	}
}

bool PeerConnectionWsClient::ParseEntry(const std::string& entry,
	std::string* name,
	int* id,
	bool* connected) {
	RTC_DCHECK(name != NULL);
	RTC_DCHECK(id != NULL);
	RTC_DCHECK(connected != NULL);
	RTC_DCHECK(!entry.empty());

	*connected = false;
	size_t separator = entry.find(',');
	if (separator != std::string::npos) {
		*id = atoi(&entry[separator + 1]);
		name->assign(entry.substr(0, separator));
		separator = entry.find(',', separator + 1);
		if (separator != std::string::npos) {
			*connected = atoi(&entry[separator + 1]) ? true : false;
		}
	}
	return !name->empty();
}

int PeerConnectionWsClient::GetResponseStatus(const std::string& response) {
	int status = -1;
	size_t pos = response.find(' ');
	if (pos != std::string::npos)
		status = atoi(&response[pos + 1]);
	return status;
}

bool PeerConnectionWsClient::ParseServerResponse(const std::string& response,
	size_t content_length,
	size_t* peer_id,
	size_t* eoh) {
	int status = GetResponseStatus(response.c_str());
	if (status != 200) {
		RTC_LOG(LS_ERROR) << "Received error from server";
		Close();
		callback_->OnDisconnected();
		return false;
	}

	*eoh = response.find("\r\n\r\n");
	RTC_DCHECK(*eoh != std::string::npos);
	if (*eoh == std::string::npos)
		return false;

	*peer_id = -1;

	// See comment in peer_channel.cc for why we use the Pragma header and
	// not e.g. "X-Peer-Id".
	GetHeaderValue(response, *eoh, "\r\nPragma: ", peer_id);

	return true;
}

void PeerConnectionWsClient::OnClose(rtc::AsyncSocket* socket, int err) {
	RTC_LOG(INFO) << __FUNCTION__;

	socket->Close();

#ifdef WIN32
	if (err != WSAECONNREFUSED) {
#else
	if (err != ECONNREFUSED) {
#endif
		if (socket == hanging_get_.get()) {
			if (state_ == CONNECTED) {
				hanging_get_->Close();
				hanging_get_->Connect(server_address_);
			}
		}
		else {
			callback_->OnMessageSent(err);
		}
	}
	else {
		if (socket == control_socket_.get()) {
			RTC_LOG(WARNING) << "Connection refused; retrying in 2 seconds";
			rtc::Thread::Current()->PostDelayed(RTC_FROM_HERE, kReconnectDelay, this,
				0);
		}
		else {
			Close();
			callback_->OnDisconnected();
		}
	}
	}

void PeerConnectionWsClient::OnMessage(rtc::Message* msg) {
	// ignore msg; there is currently only one supported message ("retry")
	DoConnect();
}
std::string PeerConnectionWsClient::RandomString(int len) {
	std::string charSet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	std::string randomString = "";
	srand((int)time(0));
	for (int i = 0; i < len; i++) {
		double rand_num=rand() / double(RAND_MAX);
		int randomPoz = rand() % charSet.length();
		//long double randomPoz = std::floor(rand_num*(charSet.length));
		randomString += charSet.substr(randomPoz, 1);
	}
	return randomString;
}


void PeerConnectionWsClient::handleMessages(char* message, size_t length) {
	//�������message
	callback_->OnMessageFromPeer(0, std::string(message);

}
