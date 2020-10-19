#include <map>
#include "NamingProxy.h"
#include "naming/NamingCommonParams.h"
#include "utils/ParamUtils.h"
#include "utils/UtilAndComs.h"
#include "utils/UuidUtils.h"
#include "utils/NetUtils.h"
#include "utils/RandomUtils.h"
#include "src/json/JSON.h"
#include "src/http/httpStatCode.h"
#include "Debug.h"
#include "NacosExceptions.h"

using namespace std;

namespace nacos{
NamingProxy::NamingProxy(HttpDelegate *httpDelegate, ServerListManager *_serverListManager, AppConfigManager *_appConfigManager) {
    log_debug("NamingProxy Constructor:\n"
              "namespace:%s, endpoint:%s, Servers:%s\n",
              _serverListManager->getNamespace().c_str(), _serverListManager->getEndpoint().c_str(),
              _serverListManager->toString().c_str());
    serverListManager = _serverListManager;
    _httpDelegate = httpDelegate;
    appConfigManager = _appConfigManager;
    serverPort = "8848";
    _http_req_timeout = atoi(appConfigManager->get(PropertyKeyConst::HTTP_REQ_TIMEOUT).c_str());

    if (serverListManager->getServerCount() == 1) {
        nacosDomain = serverListManager->getServerList().begin()->getCompleteAddress();
    }
    log_debug("The serverlist:%s\n", _serverListManager->toString().c_str());

    _hb_fail_wait = atoi(appConfigManager->get(PropertyKeyConst::HB_FAIL_WAIT_TIME).c_str());
}

NamingProxy::~NamingProxy() {
    _httpDelegate = NULL;
    appConfigManager = NULL;
    //TODO:refactor this deconstructing process to a function
    if (serverListManager != NULL) {
        delete serverListManager;
        serverListManager = NULL;
    }
}

void NamingProxy::registerService(const NacosString &serviceName, const NacosString &groupName,
                                  Instance &instance) throw(NacosException) {
    log_info("[REGISTER-SERVICE] %s registering service %s with instance: %s\n",
             getNamespaceId().c_str(), serviceName.c_str(), instance.toString().c_str());

    list <NacosString> params;
    ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
    ParamUtils::addKV(params, NamingCommonParams::SERVICE_NAME, serviceName);
    ParamUtils::addKV(params, NamingCommonParams::GROUP_NAME, groupName);
    ParamUtils::addKV(params, NamingCommonParams::CLUSTER_NAME, instance.clusterName);
    ParamUtils::addKV(params, "ip", instance.ip);
    ParamUtils::addKV(params, "port",  NacosStringOps::valueOf(instance.port));
    ParamUtils::addKV(params, "weight", NacosStringOps::valueOf(instance.weight));
    ParamUtils::addKV(params, "enable", NacosStringOps::valueOf(instance.enabled));
    ParamUtils::addKV(params, "healthy", NacosStringOps::valueOf(instance.healthy));
    ParamUtils::addKV(params, "ephemeral", NacosStringOps::valueOf(instance.ephemeral));
    //TODO:transfer metadata in JSON form
    ParamUtils::addKV(params, "metadata", JSON::toJSONString(instance.metadata));

    reqAPI(UtilAndComs::NACOS_URL_INSTANCE, params, IHttpCli::POST);
}

void NamingProxy::deregisterService(const NacosString &serviceName, Instance &instance) throw(NacosException) {
    log_info("[DEREGISTER-SERVICE] %s deregistering service %s with instance: %s\n",
             getNamespaceId().c_str(), serviceName.c_str(), instance.toString().c_str());

    list <NacosString> params;
    ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
    ParamUtils::addKV(params, NamingCommonParams::SERVICE_NAME, serviceName);
    ParamUtils::addKV(params, NamingCommonParams::CLUSTER_NAME, instance.clusterName);
    ParamUtils::addKV(params, "ip", instance.ip);
    ParamUtils::addKV(params, "port",  NacosStringOps::valueOf(instance.port));
    ParamUtils::addKV(params, "ephemeral", NacosStringOps::valueOf(instance.ephemeral));

    reqAPI(UtilAndComs::NACOS_URL_INSTANCE, params, IHttpCli::DELETE);
}

NacosString NamingProxy::queryList(const NacosString &serviceName, const NacosString &clusters, int udpPort,
                                   bool healthyOnly) throw(NacosException) {
    list <NacosString> params;
    ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
    ParamUtils::addKV(params, NamingCommonParams::SERVICE_NAME, serviceName);
    ParamUtils::addKV(params, "clusters", clusters);
    ParamUtils::addKV(params, "udpPort", NacosStringOps::valueOf(udpPort));
    ParamUtils::addKV(params, "clientIP", NetUtils::localIP());
    ParamUtils::addKV(params, "healthyOnly", NacosStringOps::valueOf(healthyOnly));

    return reqAPI(UtilAndComs::NACOS_URL_BASE + "/instance/list", params, IHttpCli::GET);
}

NacosString
NamingProxy::reqAPI(const NacosString &api, list <NacosString> &params, int method) throw(NacosException) {
    ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
    list <NacosServerInfo> servers = serverListManager->getServerList();

    if (serverListManager->getServerCount() == 0) {
        throw NacosException(NacosException::NO_SERVER_AVAILABLE, "no server available");
    }

    NacosString errmsg;
    if (!servers.empty()) {
        size_t maxSvrSlot = servers.size();
        log_debug("nr_servers:%d\n", maxSvrSlot);
        size_t selectedServer = RandomUtils::random(0, maxSvrSlot) % maxSvrSlot;
        log_debug("selected_server:%d\n", selectedServer);

        for (size_t i = 0; i < servers.size(); i++) {
            NacosServerInfo server = ParamUtils::getNthElem(servers, selectedServer);
            log_debug("Trying to access server:%s\n", server.toString().c_str());
            try {
                return callServer(api, params, server.getCompleteAddress(), method);
            }
            catch (NacosException &e) {
                errmsg = e.what();
                log_error("request %s failed.\n", server.toString().c_str());
            }
            catch (exception &e) {
                errmsg = e.what();
                log_error("request %s failed.\n", server.toString().c_str());
            }

            selectedServer = (selectedServer + 1) % servers.size();
        }

        throw NacosException(NacosException::ALL_SERVERS_TRIED_AND_FAILED, "failed to req API:" + api + " after all servers(" + serverListManager->toString() +
                                ") tried: "
                                + errmsg);
    }

    for (int i = 0; i < UtilAndComs::REQUEST_DOMAIN_RETRY_COUNT; i++) {
        try {
            return callServer(api, params, nacosDomain);
        }
        catch (exception &e) {
            errmsg = e.what();
            log_error("[NA] req api:%s failed, server(%s), e = %s\n", api.c_str(), nacosDomain.c_str(), e.what());
        }
    }

    throw NacosException(NacosException::ALL_SERVERS_TRIED_AND_FAILED, "failed to req API:/api/" + api + " after all servers(" + serverListManager->toString() +
                            ") tried: " + errmsg);
}

NacosString NamingProxy::callServer
        (
                const NacosString &api,
                list <NacosString> &params,
                const NacosString &curServer
        ) throw(NacosException) {
    return callServer(api, params, nacosDomain, IHttpCli::GET);
}

NacosString NamingProxy::callServer
        (
                const NacosString &api,
                list <NacosString> &params,
                const NacosString &curServer,
                int method
        ) throw(NacosException) {
    NacosString requestUrl;
    //Current server address doesn't have SERVER_ADDR_IP_SPLITER, which means
    if (!ParamUtils::contains(curServer, UtilAndComs::SERVER_ADDR_IP_SPLITER)) {
        requestUrl = curServer + UtilAndComs::SERVER_ADDR_IP_SPLITER + serverPort;
    } else {
        requestUrl = curServer;
    }

    //TODO:http/https implementation
    requestUrl = "http://" + requestUrl + api;

    HttpResult requestRes;
    list <NacosString> headers;
    headers = builderHeaders();

    try {
        switch (method) {
            case IHttpCli::GET:
                requestRes = _httpDelegate->httpGet(requestUrl, headers, params, UtilAndComs::ENCODING,
                                              _http_req_timeout);
                break;
            case IHttpCli::PUT:
                requestRes = _httpDelegate->httpPut(requestUrl, headers, params, UtilAndComs::ENCODING,
                                              _http_req_timeout);
                break;
            case IHttpCli::POST:
                requestRes = _httpDelegate->httpPost(requestUrl, headers, params, UtilAndComs::ENCODING,
                                               _http_req_timeout);
                break;
            case IHttpCli::DELETE:
                requestRes = _httpDelegate->httpDelete(requestUrl, headers, params, UtilAndComs::ENCODING,
                                                 _http_req_timeout);
                break;
        }
    }
    catch (NetworkException &e) {
        NacosString errMsg = "Failed to request server, ";
        errMsg += e.what();
        throw NacosException(NacosException::SERVER_ERROR, errMsg);
    }

    if (requestRes.code == HTTP_OK) {
        return requestRes.content;
    }

    if (requestRes.code == HTTP_NOT_MODIFIED) {
        return NULLSTR;
    }
    //TODO:Metrics & Monitoring

    throw NacosException(requestRes.code,
                         "failed to req API:" + requestUrl + " code:" + NacosStringOps::valueOf(requestRes.code) +
                         " errormsg:" + requestRes.content);
}

inline NacosString NamingProxy::getNamespaceId() {
    return appConfigManager->get(PropertyKeyConst::NAMESPACE);
}

list <NacosString> NamingProxy::builderHeaders() {
    list <NacosString> headers;
    headers.push_back("Client-Version");
    headers.push_back(UtilAndComs::VERSION);

    headers.push_back("User-Agent");
    headers.push_back(UtilAndComs::VERSION);

    headers.push_back("Accept-Encoding");
    headers.push_back("gzip,deflate,sdch");

    headers.push_back("Connection");
    headers.push_back("Keep-Alive");

    headers.push_back("RequestId");
    headers.push_back(UuidUtils::generateUuid());

    headers.push_back("Request-Module");
    headers.push_back("Naming");
    return headers;
}

long NamingProxy::sendBeat(BeatInfo &beatInfo) {
    try {
        NacosString beatInfoStr = beatInfo.toString();
        log_info("[BEAT] %s sending beat to server: %s\n", getNamespaceId().c_str(), beatInfoStr.c_str());
        list <NacosString> params;
        ParamUtils::addKV(params, NamingCommonParams::BEAT, JSON::toJSONString(beatInfo));
        ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
        ParamUtils::addKV(params, NamingCommonParams::SERVICE_NAME, beatInfo.serviceName);
        NacosString result = reqAPI(UtilAndComs::NACOS_URL_BASE + "/instance/beat", params, IHttpCli::PUT);
        //JSONObject jsonObject = JSON.parseObject(result);

        if (!isNull(result)) {
            return JSON::getLong(result, "clientBeatInterval");
        }
    }
    catch (NacosException &e) {
        NacosString jsonBeatInfo = JSON::toJSONString(beatInfo);
        log_error("[CLIENT-BEAT] failed to send beat: %s e:%s\n", jsonBeatInfo.c_str(), e.what());
        return _hb_fail_wait;
    }
    return 0L;
}

ListView<NacosString> NamingProxy::getServiceList(int page, int pageSize, const NacosString &groupName) throw(NacosException)
{
    log_debug("[NAMEPRXY] request:group=%s page=%d pageSize=%d\n", groupName.c_str(), page, pageSize);
    list <NacosString> params;
    ParamUtils::addKV(params, NamingCommonParams::PAGE_NO, NacosStringOps::valueOf(page));
    ParamUtils::addKV(params, NamingCommonParams::PAGE_SIZE, NacosStringOps::valueOf(pageSize));
    ParamUtils::addKV(params, NamingCommonParams::GROUP_NAME, groupName);
    ParamUtils::addKV(params, NamingCommonParams::NAMESPACE_ID, getNamespaceId());
    NacosString result = reqAPI(UtilAndComs::NACOS_URL_BASE + "/service/list", params, IHttpCli::GET);

    if (!isNull(result)) {
        return JSON::Json2ServiceList(result);
    }

    ListView<NacosString> nullResult;
    nullResult.setCount(0);
    return nullResult;
}


bool NamingProxy::serverHealthy() {
    list<NacosString> params;
    NacosString result = reqAPI(UtilAndComs::NACOS_URL_BASE + "/operator/metrics", params, IHttpCli::GET);
    NacosString healthyTag = "status\":\"";
    size_t pos = result.find(healthyTag);
    if (pos == std::string::npos) {
        return false;
    }

    NacosString healthy = result.substr(pos + healthyTag.length(), 2);
    return healthy == "UP";
}
}//namespace nacos
