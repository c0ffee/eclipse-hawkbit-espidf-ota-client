/*******************************************************************************
 * Copyright (c) 2020 Red Hat Inc
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0
 *
 * SPDX-License-Identifier: EPL-2.0
 *******************************************************************************/
#include "hawkbit.h"

static const char* TAG = "HAWKBIT";

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                if (output_buffer != NULL) {
                    free(output_buffer);
                    output_buffer = NULL;
                }
                output_len = 0;
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

HawkbitClient::HawkbitClient(
    JsonDocument& doc,
    const std::string& baseUrl,
    const std::string& tenantName,
    const std::string& controllerId,
    const std::string &securityToken,
    char *server_cert_pem_start) :
    _doc(doc),
    _baseUrl(baseUrl),
    _tenantName(tenantName),
    _controllerId(controllerId),
    _authToken("TargetToken " + securityToken)
{
    _http_config.event_handler = _http_event_handler;
    _http_config.user_data = resultPayload;        // Pass address of local buffer to get response
    _http_config.disable_auto_redirect = false;
    _http_config.cert_pem = server_cert_pem_start;
}

UpdateResult HawkbitClient::updateRegistration(const Registration& registration, const std::map<std::string,std::string>& data, MergeMode mergeMode, std::initializer_list<std::string> details)
{
    _doc.clear();

    switch(mergeMode) {
        case MERGE:
            _doc["mode"] = "merge";
            break;
        case REPLACE:
            _doc["mode"] = "replace";
            break;
        case REMOVE:
            _doc["mode"] = "remove";
            break;
    }

    _doc.createNestedObject("data");
    for (const std::pair<std::string,std::string>& entry : data) {
        _doc["data"][std::string(entry.first)] = entry.second;
    }

    JsonArray d = _doc["status"].createNestedArray("details");
    for (auto detail : details) {
        d.add(detail);
    }

    _doc["status"]["execution"] = "closed";
    _doc["status"]["result"]["finished"] = "success";

    _http.begin(this->_wifi, registration.url());

    _http.addHeader("Accept", "application/hal+json");
    _http.addHeader("Content-Type", "application/json");
    _http.addHeader("Authorization", this->_authToken);

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_DEBUG
    serializeJsonPretty(_doc, Serial);
#endif

    String buffer;
    size_t len = serializeJson(_doc, buffer);
    (void)len; // ignore unused

    log_d("JSON - len: %d", len);

    int code = _http.PUT(buffer);
    log_d("Result - code: %d", code);

    String resultPayload = _http.getString();
    log_d("Result - payload: %s", resultPayload.c_str());

    _http.end();

    return UpdateResult(code);
}

State HawkbitClient::readState()
{
    _http.begin(this->_wifi, this->_baseUrl + "/" + this->_tenantName + "/controller/v1/" + this->_controllerId);

    _http.addHeader("Authorization", this->_authToken);
    _http.addHeader("Accept", "application/hal+json");

    _doc.clear();

    int code = _http.GET();
    log_d("Result - code: %d", code);
    String resultPayload = _http.getString();
    log_d("Result - payload: %s", resultPayload.c_str());
    if ( code == HTTP_CODE_OK ) {
        DeserializationError error = deserializeJson(_doc, resultPayload);
        if (error) {
            _http.end();
            // FIXME: need a way to handle errors
            throw 1;
        }
    }
    _http.end();

    String href = _doc["_links"]["deploymentBase"]["href"] | "";
    if (!href.isEmpty()) {
        log_d("Fetching deployment: %s", href.c_str());
        return State(this->readDeployment(href));
    }

    href = _doc["_links"]["configData"]["href"] | "";
    if (!href.isEmpty()) {
        log_d("Need to register", href.c_str());
        return State(Registration(href));
    }

    href = _doc["_links"]["cancelAction"]["href"] | "";
    if (!href.isEmpty()) {
        log_d("Fetching cancel action: %s", href.c_str());
        return State(this->readCancel(href));
    }

    log_d("No update");
    return State();
}

std::map<std::string,std::string> toMap(const JsonObject& obj) {
    std::map<std::string,std::string> result;
    for (const JsonPair& p: obj) {
        if (p.value().is<char*>()) {
            result[std::string(p.key().c_str())] = std::string(p.value().as<char*>());
        }
    }
    return result;
}

std::map<std::string,std::string> toLinks(const JsonObject& obj) {
    std::map<std::string,std::string> result;
    for (const JsonPair& p: obj) {
        const char* key = p.key().c_str();
        const char* value = p.value()["href"];
        result[std::string(key)] = std::string(value);
    }
    return result;
}

std::list<Artifact> artifacts(const JsonArray& artifacts)
{
    std::list<Artifact> result;

    for (JsonObject o : artifacts) {
        Artifact artifact (
            o["filename"],
            o["size"] | 0,
            toMap(o["hashes"]),
            toLinks(o["_links"])
        );
        result.push_back(artifact);
    }

    return result;
}

std::list<Chunk> chunks(const JsonArray& chunks)
{
    std::list<Chunk> result;

    for(JsonObject o : chunks)
    {
        Chunk chunk(
            o["part"],
            o["version"],
            o["name"],
            artifacts(o["artifacts"])
            );
        result.push_back(chunk);
    }

    return result;
}

Deployment HawkbitClient::readDeployment(const std::string& href)
{
    _http.begin(this->_wifi, href);

    _http.addHeader("Authorization", this->_authToken);
    _http.addHeader("Accept", "application/hal+json");

    _doc.clear();

    int code = _http.GET();
    log_d("Result - code: %d", code);
    String resultPayload = _http.getString();
    log_d("Result - payload: %s", resultPayload.c_str());
    if ( code == HTTP_CODE_OK ) {
        DeserializationError error = deserializeJson(_doc, resultPayload);
        if (error) {
            _http.end();
            // FIXME: need a way to handle errors
            throw 1;
        }
    }
    _http.end();

    String id = _doc["id"];
    String download = _doc["deployment"]["download"];
    String update = _doc["deployment"]["update"];

    return Deployment(id, download, update, chunks(_doc["deployment"]["chunks"]));
}

Stop HawkbitClient::readCancel(const std::string& href)
{
    _http.begin(this->_wifi, href);

    _http.addHeader("Authorization", this->_authToken);
    _http.addHeader("Accept", "application/hal+json");

    _doc.clear();

    int code = _http.GET();
    log_d("Result - code: %d", code);
    String resultPayload = _http.getString();
    log_d("Result - payload: %s", resultPayload.c_str());
    if ( code == HTTP_CODE_OK ) {
        DeserializationError error = deserializeJson(_doc, resultPayload);
        if (error) {
            _http.end();
            // FIXME: need a way to handle errors
            throw 1;
        }
    }
    _http.end();

    String stopId = _doc["cancelAction"]["stopId"] | "";
    
    return Stop(stopId);
}

std::string HawkbitClient::feedbackUrl(const Deployment& deployment) const
{
    return this->_baseUrl + "/" + this->_tenantName + "/controller/v1/" + this->_controllerId + "/deploymentBase/" + deployment.id() + "/feedback";
}

std::string HawkbitClient::feedbackUrl(const Stop& stop) const
{
    return this->_baseUrl + "/" + this->_tenantName + "/controller/v1/" + this->_controllerId + "/cancelAction/" + stop.id() + "/feedback";
}

template<typename IdProvider>
UpdateResult HawkbitClient::sendFeedback(IdProvider id, const std::string& execution, const std::string& finished, std::vector<std::string> details)
{
    _doc.clear();

    _doc["id"] = id.id();
    
    JsonArray d = _doc["status"].createNestedArray("details");
    for (auto detail : details) {
        d.add(detail);
    }

    _doc["status"]["execution"] = execution;
    _doc["status"]["result"]["finished"] = finished;

    _http.begin(this->_wifi, this->feedbackUrl(id));

    _http.addHeader("Accept", "application/hal+json");
    _http.addHeader("Content-Type", "application/json");
    _http.addHeader("Authorization", this->_authToken);

    String buffer;
    size_t len = serializeJson(_doc, buffer);
    (void)len; // ignore unused

    log_d("JSON - len: %d", len);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_DEBUG
    serializeJsonPretty(_doc, Serial);
#endif

    // FIXME: handle result
    int code = _http.POST(buffer);
    log_d("Result - code: %d", code);

    String resultPayload = _http.getString();
    log_d("Result - payload: %s", resultPayload.c_str());

    _http.end();

    return UpdateResult(code);
}

UpdateResult HawkbitClient::reportProgress(const Deployment& deployment, uint32_t done, uint32_t total, std::vector<std::string> details)
{
    return sendFeedback(
        deployment,
        "proceeding",
        "none",
        details
    );
}

UpdateResult HawkbitClient::reportScheduled(const Deployment& deployment, std::vector<std::string> details)
{
    return sendFeedback(
        deployment,
        "scheduled",
        "none",
        details
    );
}

UpdateResult HawkbitClient::reportResumed(const Deployment& deployment, std::vector<std::string> details)
{
    return sendFeedback(
        deployment,
        "resumed",
        "none",
        details
    );
}

UpdateResult HawkbitClient::reportComplete(const Deployment& deployment, bool success, std::vector<std::string> details)
{
    return sendFeedback(
        deployment,
        "closed",
        success ? "success" : "failure",
        details
    );
}

UpdateResult HawkbitClient::reportCanceled(const Deployment& deployment, std::vector<std::string> details)
{
    return sendFeedback(
        deployment,
        "canceled",
        "none",
        details
    );
}

UpdateResult HawkbitClient::reportCancelAccepted(const Stop& stop, std::vector<std::string> details)
{
    return sendFeedback(
        stop,
        "closed",
        "success",
        details
    );
}

UpdateResult HawkbitClient::reportCancelRejected(const Stop& stop, std::vector<std::string> details)
{
    return sendFeedback(
        stop,
        "closed",
        "failure",
        details
    );
}
