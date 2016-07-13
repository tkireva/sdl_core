/*
 * Copyright (c) 2016, Ford Motor Company
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided with the
 * distribution.
 *
 * Neither the name of the Ford Motor Company nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "application_manager/commands/hmi/sdl_activate_app_request.h"
#include "application_manager/state_controller.h"
#include "application_manager/message_helper.h"

namespace application_manager {

namespace commands {

namespace {
struct SDL4AppsOnDevice
    : std::unary_function<ApplicationSharedPtr , bool> {
  connection_handler::DeviceHandle handle_;
  SDL4AppsOnDevice(const connection_handler::DeviceHandle handle)
      : handle_(handle) {}
  bool operator()(const ApplicationSharedPtr app) const {
    return app
               ? handle_ == app->device() &&
                     ProtocolVersion::kV4 == app->protocol_version()
               : false;
  }
};

struct ForegroundApp
    : std::unary_function<SDLActivateAppRequest::SDL4Apps::value_type, bool> {
  bool operator()(const SDLActivateAppRequest::SDL4Apps::value_type ptr) const {
    return ptr.valid() ? ptr->is_foreground() : false;
  }
};

struct SendLaunchApp
    : std::unary_function<SDLActivateAppRequest::SDL4Apps::value_type, bool> {
  ApplicationConstSharedPtr app_to_launch_;
  ApplicationManager& application_manager_;
  SendLaunchApp(ApplicationConstSharedPtr app_to_launch, ApplicationManager& am)
      : app_to_launch_(app_to_launch), application_manager_(am) {}
  bool operator()(const SDLActivateAppRequest::SDL4Apps::value_type ptr) const {
    MessageHelper::SendLaunchApp((*ptr).app_id(),
                                 app_to_launch_->SchemaUrl(),
                                 app_to_launch_->PackageName(),
                                 application_manager_);
    return true;
  }
};
}

SDLActivateAppRequest::SDLActivateAppRequest(
    const MessageSharedPtr& message, ApplicationManager& application_manager)
    : RequestFromHMI(message, application_manager) {}

SDLActivateAppRequest::~SDLActivateAppRequest() {}

void SDLActivateAppRequest::Run() {
  LOG4CXX_AUTO_TRACE(logger_);
  using namespace hmi_apis::FunctionID;
  using namespace hmi_apis::Common_Result;

  const uint32_t application_id = app_id();

  ApplicationConstSharedPtr app_to_activate =
      application_manager_.application(application_id);

  if (!app_to_activate) {
    LOG4CXX_WARN(
        logger_,
        "Can't find application within regular apps: " << application_id);

    // Here is the hack - in fact SDL gets hmi_app_id in appID field and
    // replaces it with connection_key only for normally registered apps, but
    // for apps_to_be_registered (waiting) it keeps original value (hmi_app_id)
    // so method does lookup for hmi_app_id
    app_to_activate = application_manager_.app_to_be_registered(application_id);

    if (!app_to_activate) {
      LOG4CXX_WARN(
          logger_,
          "Can't find application within waiting apps: " << application_id);
      return;
    }
  }

  LOG4CXX_DEBUG(logger_,
                "Found application to activate. Application id is "
                    << app_to_activate->app_id());

  if (app_to_activate->IsRegistered()) {
    LOG4CXX_DEBUG(logger_, "Application is registered. Activating.");
    application_manager_.GetPolicyHandler().OnActivateApp(application_id,
                                                          correlation_id());
    return;
  }

  connection_handler::DeviceHandle device_handle = app_to_activate->device();
  ApplicationSharedPtr foreground_sdl4_app = get_foreground_app(device_handle);
  SDL4Apps sdl4_apps = get_sdl4_apps(device_handle);

  if (!foreground_sdl4_app.valid() && sdl4_apps.empty()) {
    LOG4CXX_ERROR(logger_,
                  "Can't find regular foreground app with the same "
                  "connection id:"
                      << device_handle);
    SendResponse(false, correlation_id(), SDL_ActivateApp, NO_APPS_REGISTERED);
    return;
  }

  LOG4CXX_DEBUG(logger_,
                "Application is not registered yet. "
                "Sending launch request.");

  if (foreground_sdl4_app.valid()) {
    LOG4CXX_DEBUG(logger_, "Sending request to foreground application.");
    MessageHelper::SendLaunchApp(foreground_sdl4_app->app_id(),
                                 app_to_activate->SchemaUrl(),
                                 app_to_activate->PackageName(),
                                 application_manager_);
  } else {
    LOG4CXX_DEBUG(logger_,
                  "No preffered (foreground) application is found. "
                  "Sending request to all v4 applications.");
    std::for_each(sdl4_apps.begin(),
                  sdl4_apps.end(),
                  SendLaunchApp(app_to_activate, application_manager_));
  }

  subscribe_on_event(BasicCommunication_OnAppRegistered);
}

void SDLActivateAppRequest::onTimeOut() {
  using namespace hmi_apis::FunctionID;
  using namespace hmi_apis::Common_Result;
  using namespace application_manager;
  unsubscribe_from_event(BasicCommunication_OnAppRegistered);
  SendResponse(
      false, correlation_id(), SDL_ActivateApp, APPLICATION_NOT_REGISTERED);
}

void SDLActivateAppRequest::on_event(const event_engine::Event& event) {
  using namespace hmi_apis::FunctionID;
  if (event.id() != BasicCommunication_OnAppRegistered) {
    return;
  }
  unsubscribe_from_event(BasicCommunication_OnAppRegistered);

  // Have to use HMI app id from event, since HMI app id from original request
  // message will be changed after app, initially requested for launch via
  // SDL.ActivateApp, will be registered
  const uint32_t hmi_application_id = hmi_app_id(event.smart_object());

  ApplicationSharedPtr app =
      application_manager_.application_by_hmi_app(hmi_application_id);
  if (!app) {
    LOG4CXX_ERROR(
        logger_, "Application not found by HMI app id: " << hmi_application_id);
    return;
  }
  application_manager_.GetPolicyHandler().OnActivateApp(app->app_id(),
                                                        correlation_id());
}

uint32_t SDLActivateAppRequest::app_id() const {
  using namespace strings;
  if (!(*message_).keyExists(msg_params)) {
    LOG4CXX_DEBUG(logger_, msg_params << " section is absent in the message.");
    return 0;
  }
  if (!(*message_)[msg_params].keyExists(strings::app_id)) {
    LOG4CXX_DEBUG(logger_,
                  strings::app_id << " section is absent in the message.");
    return 0;
  }
  return (*message_)[msg_params][strings::app_id].asUInt();
}

uint32_t SDLActivateAppRequest::hmi_app_id(
    const smart_objects::SmartObject& so) const {
  using namespace strings;
  if (!so.keyExists(params)) {
    LOG4CXX_DEBUG(logger_, params << " section is absent in the message.");
    return 0;
  }
  if (!so[msg_params].keyExists(application)) {
    LOG4CXX_DEBUG(logger_, application << " section is absent in the message.");
    return 0;
  }
  if (so[msg_params][application].keyExists(strings::app_id)) {
    LOG4CXX_DEBUG(logger_,
                  strings::app_id << " section is absent in the message.");
    return 0;
  }
  return so[msg_params][application][strings::app_id].asUInt();
}

SDLActivateAppRequest::SDL4Apps SDLActivateAppRequest::get_sdl4_apps(
    const connection_handler::DeviceHandle handle) const {
  const ApplicationSet app_list = application_manager_.applications().GetData();
  SDL4Apps sdl4_apps;
  std::copy_if(app_list.begin(),
               app_list.end(),
               std::back_inserter(sdl4_apps),
               SDL4AppsOnDevice(handle));
  return sdl4_apps;
}

ApplicationSharedPtr SDLActivateAppRequest::get_foreground_app(
    const connection_handler::DeviceHandle handle) const {
  SDL4Apps sdl4_apps = get_sdl4_apps(handle);
  SDL4Apps::iterator foreground_app =
      std::find_if(sdl4_apps.begin(), sdl4_apps.end(), ForegroundApp());
  return foreground_app != sdl4_apps.end() ? *foreground_app
                                           : ApplicationSharedPtr();
}

}  // namespace commands
}  // namespace application_manager
