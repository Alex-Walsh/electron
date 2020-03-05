// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <string>

#include "base/task/post_task.h"
#include "base/values.h"
#include "content/public/renderer/render_frame.h"
#include "gin/dictionary.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/wrappable.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "shell/common/api/api.mojom.h"
#include "shell/common/gin_converters/blink_converter.h"
#include "shell/common/gin_converters/value_converter.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/node_bindings.h"
#include "shell/common/node_includes.h"
#include "shell/common/v8_value_serializer.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_message_port_converter.h"

using blink::WebLocalFrame;
using content::RenderFrame;

namespace {

RenderFrame* GetCurrentRenderFrame() {
  WebLocalFrame* frame = WebLocalFrame::FrameForCurrentContext();
  if (!frame)
    return nullptr;

  return RenderFrame::FromWebFrame(frame);
}

class IPCRenderer : public gin::Wrappable<IPCRenderer> {
 public:
  static gin::WrapperInfo kWrapperInfo;

  static gin::Handle<IPCRenderer> Create(v8::Isolate* isolate) {
    return gin::CreateHandle(isolate, new IPCRenderer(isolate));
  }

  explicit IPCRenderer(v8::Isolate* isolate) {
    RenderFrame* render_frame = GetCurrentRenderFrame();
    DCHECK(render_frame);

    render_frame->GetRemoteInterfaces()->GetInterface(
        mojo::MakeRequest(&electron_browser_ptr_));
  }

  // gin::Wrappable:
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override {
    return gin::Wrappable<IPCRenderer>::GetObjectTemplateBuilder(isolate)
        .SetMethod("send", &IPCRenderer::Send)
        .SetMethod("sendSync", &IPCRenderer::SendSync)
        .SetMethod("sendTo", &IPCRenderer::SendTo)
        .SetMethod("sendToHost", &IPCRenderer::SendToHost)
        .SetMethod("invoke", &IPCRenderer::Invoke)
        .SetMethod("postMessage", &IPCRenderer::PostMessage);
  }

  const char* GetTypeName() override { return "IPCRenderer"; }

 private:
  void Send(v8::Isolate* isolate,
            bool internal,
            const std::string& channel,
            v8::Local<v8::Value> arguments) {
    blink::CloneableMessage message;
    if (!electron::SerializeV8Value(isolate, arguments, &message)) {
      return;
    }
    electron_browser_ptr_->Message(internal, channel, std::move(message));
  }

  v8::Local<v8::Promise> Invoke(v8::Isolate* isolate,
                                bool internal,
                                const std::string& channel,
                                v8::Local<v8::Value> arguments) {
    blink::CloneableMessage message;
    if (!electron::SerializeV8Value(isolate, arguments, &message)) {
      return v8::Local<v8::Promise>();
    }
    gin_helper::Promise<blink::CloneableMessage> p(isolate);
    auto handle = p.GetHandle();

    electron_browser_ptr_->Invoke(
        internal, channel, std::move(message),
        base::BindOnce(
            [](gin_helper::Promise<blink::CloneableMessage> p,
               blink::CloneableMessage result) { p.Resolve(result); },
            std::move(p)));

    return handle;
  }

  void PostMessage(v8::Isolate* isolate,
                   const std::string& channel,
                   v8::Local<v8::Value> message_value,
                   base::Optional<v8::Local<v8::Value>> transfer) {
    blink::TransferableMessage transferable_message;
    if (!electron::SerializeV8Value(isolate, message_value,
                                    &transferable_message)) {
      // SerializeV8Value sets an exception.
      return;
    }

    std::vector<v8::Local<v8::Object>> transferables;
    if (transfer) {
      if (!gin::ConvertFromV8(isolate, *transfer, &transferables)) {
        isolate->ThrowException(v8::Exception::Error(
            gin::StringToV8(isolate, "Invalid value for transfer")));
        return;
      }
    }

    std::vector<blink::MessagePortChannel> ports;
    for (auto& transferable : transferables) {
      base::Optional<blink::MessagePortChannel> port =
          blink::WebMessagePortConverter::
              DisentangleAndExtractMessagePortChannel(isolate, transferable);
      if (!port.has_value()) {
        isolate->ThrowException(v8::Exception::Error(
            gin::StringToV8(isolate, "Invalid value for transfer")));
        return;
      }
      ports.emplace_back(port.value());
    }

    transferable_message.ports = std::move(ports);
    electron_browser_ptr_->ReceivePostMessage(channel,
                                              std::move(transferable_message));
  }

  void SendTo(v8::Isolate* isolate,
              bool internal,
              bool send_to_all,
              int32_t web_contents_id,
              const std::string& channel,
              v8::Local<v8::Value> arguments) {
    blink::CloneableMessage message;
    if (!electron::SerializeV8Value(isolate, arguments, &message)) {
      return;
    }
    electron_browser_ptr_->MessageTo(internal, send_to_all, web_contents_id,
                                     channel, std::move(message));
  }

  void SendToHost(v8::Isolate* isolate,
                  const std::string& channel,
                  v8::Local<v8::Value> arguments) {
    blink::CloneableMessage message;
    if (!electron::SerializeV8Value(isolate, arguments, &message)) {
      return;
    }
    electron_browser_ptr_->MessageHost(channel, std::move(message));
  }

  v8::Local<v8::Value> SendSync(v8::Isolate* isolate,
                                bool internal,
                                const std::string& channel,
                                v8::Local<v8::Value> arguments) {
    blink::CloneableMessage message;
    if (!electron::SerializeV8Value(isolate, arguments, &message)) {
      return v8::Local<v8::Value>();
    }

    blink::CloneableMessage result;
    electron_browser_ptr_->MessageSync(internal, channel, std::move(message),
                                       &result);
    return electron::DeserializeV8Value(isolate, result);
  }

  electron::mojom::ElectronBrowserPtr electron_browser_ptr_;
};

gin::WrapperInfo IPCRenderer::kWrapperInfo = {gin::kEmbedderNativeGin};

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin::Dictionary dict(context->GetIsolate(), exports);
  dict.Set("ipc", IPCRenderer::Create(context->GetIsolate()));
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(electron_renderer_ipc, Initialize)
