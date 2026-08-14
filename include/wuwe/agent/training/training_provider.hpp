#ifndef WUWE_AGENT_TRAINING_TRAINING_PROVIDER_HPP
#define WUWE_AGENT_TRAINING_TRAINING_PROVIDER_HPP

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/training/training_context.hpp>
#include <wuwe/agent/training/training_core.hpp>
#include <wuwe/agent/training/training_error.hpp>
#include <wuwe/agent/training/training_json.hpp>
#include <wuwe/agent/training/training_policy.hpp>
#include <wuwe/agent/training/training_store.hpp>

namespace wuwe::agent::training {

class training_provider {
public:
  virtual ~training_provider() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual training_result<training_provider_snapshot> submit(
    const training_request& request, const training_context& context) = 0;
  [[nodiscard]] virtual training_result<training_provider_snapshot> inspect(
    const std::string& provider_job_id, const training_context& context) = 0;
  [[nodiscard]] virtual training_result<std::optional<training_submission_record>>
  lookup_submission(const std::string& idempotency_key, const training_context& context) {
    (void)idempotency_key;
    (void)context;
    return training_result<std::optional<training_submission_record>>::failure({
      .code = training_errc::unsupported_operation,
      .message = "training provider does not support submission lookup",
    });
  }
  [[nodiscard]] virtual training_result<training_provider_snapshot> cancel(
    const std::string& provider_job_id, const training_context& context) = 0;
  [[nodiscard]] virtual training_result<training_provider_snapshot> resume(
    const std::string& provider_job_id, const training_checkpoint& checkpoint,
    const training_context& context) {
    (void)provider_job_id;
    (void)checkpoint;
    (void)context;
    return training_result<training_provider_snapshot>::failure({
      .code = training_errc::unsupported_operation,
      .message = "training provider does not support resume",
      .remote_outcome = training_remote_outcome::not_applied,
    });
  }
};

class function_training_provider final : public training_provider {
public:
  using raw_submit_callback =
    std::function<training_provider_snapshot(const training_request&, const training_context&)>;
  using raw_inspect_callback =
    std::function<training_provider_snapshot(const std::string&, const training_context&)>;
  using raw_cancel_callback = raw_inspect_callback;
  using raw_resume_callback = std::function<training_provider_snapshot(
    const std::string&, const training_checkpoint&, const training_context&)>;
  using submit_callback = std::function<training_result<training_provider_snapshot>(
    const training_request&, const training_context&)>;
  using inspect_callback = std::function<training_result<training_provider_snapshot>(
    const std::string&, const training_context&)>;
  using cancel_callback = inspect_callback;
  using lookup_callback = std::function<training_result<std::optional<training_submission_record>>(
    const std::string&, const training_context&)>;
  using resume_callback = std::function<training_result<training_provider_snapshot>(
    const std::string&, const training_checkpoint&, const training_context&)>;

  function_training_provider(std::string provider_name, submit_callback submit,
    inspect_callback inspect, cancel_callback cancel, resume_callback resume = {},
    lookup_callback lookup = {})
      : name_(std::move(provider_name)), submit_(std::move(submit)), inspect_(std::move(inspect)),
        cancel_(std::move(cancel)), resume_(std::move(resume)), lookup_(std::move(lookup)) {
    if (name_.empty() || !submit_ || !inspect_ || !cancel_)
      throw std::invalid_argument("function training provider requires name and callbacks");
  }

  function_training_provider(std::string provider_name, raw_submit_callback submit,
    raw_inspect_callback inspect, raw_cancel_callback cancel, raw_resume_callback resume = {})
      : function_training_provider(std::move(provider_name),
          [callback = std::move(submit)](const training_request& request,
            const training_context& context) {
            try {
              return training_result<training_provider_snapshot>::success(callback(request, context));
            }
            catch (const std::exception& ex) {
              return training_result<training_provider_snapshot>::failure({
                .code = training_errc::provider_transport,
                .message = callback_exception_message(ex), .retryable = true,
                .remote_outcome = training_remote_outcome::uncertain });
            }
            catch (...) { return unknown_callback_failure(training_remote_outcome::uncertain); }
          },
          [callback = std::move(inspect)](const std::string& id,
            const training_context& context) {
            try {
              return training_result<training_provider_snapshot>::success(callback(id, context));
            }
            catch (const std::exception& ex) {
              return training_result<training_provider_snapshot>::failure({
                .code = training_errc::provider_transport,
                .message = callback_exception_message(ex), .retryable = true });
            }
            catch (...) { return unknown_callback_failure(); }
          },
          [callback = std::move(cancel)](const std::string& id,
            const training_context& context) {
            try {
              return training_result<training_provider_snapshot>::success(callback(id, context));
            }
            catch (const std::exception& ex) {
              return training_result<training_provider_snapshot>::failure({
                .code = training_errc::provider_transport,
                .message = callback_exception_message(ex), .retryable = true,
                .remote_outcome = training_remote_outcome::uncertain });
            }
            catch (...) { return unknown_callback_failure(training_remote_outcome::uncertain); }
          },
          resume_callback(resume ? [callback = std::move(resume)](const std::string& id,
                            const training_checkpoint& checkpoint, const training_context& context) {
            try {
              return training_result<training_provider_snapshot>::success(
                callback(id, checkpoint, context));
            }
            catch (const std::exception& ex) {
              return training_result<training_provider_snapshot>::failure({
                .code = training_errc::provider_transport,
                .message = callback_exception_message(ex), .retryable = true,
                .remote_outcome = training_remote_outcome::uncertain });
            }
            catch (...) { return unknown_callback_failure(training_remote_outcome::uncertain); }
          } : resume_callback {})) {
  }

  [[nodiscard]] std::string name() const override {
    return name_;
  }

  [[nodiscard]] training_result<training_provider_snapshot> submit(
    const training_request& request, const training_context& context) override {
    return invoke(submit_, training_remote_outcome::uncertain, request, context);
  }

  [[nodiscard]] training_result<training_provider_snapshot> inspect(
    const std::string& provider_job_id, const training_context& context) override {
    return invoke(inspect_, training_remote_outcome::not_applicable, provider_job_id, context);
  }

  [[nodiscard]] training_result<training_provider_snapshot> cancel(
    const std::string& provider_job_id, const training_context& context) override {
    return invoke(cancel_, training_remote_outcome::uncertain, provider_job_id, context);
  }

  [[nodiscard]] training_result<std::optional<training_submission_record>> lookup_submission(
    const std::string& idempotency_key, const training_context& context) override {
    if (!lookup_)
      return training_provider::lookup_submission(idempotency_key, context);
    training_result<std::optional<training_submission_record>> result =
      training_result<std::optional<training_submission_record>>::failure({
        .code = training_errc::provider_transport,
        .message = "training provider lookup callback did not respond",
      });
    try {
      result = lookup_(idempotency_key, context);
    }
    catch (const std::exception& ex) {
      return training_result<std::optional<training_submission_record>>::failure(
        { .code = training_errc::provider_transport,
          .message = callback_exception_message(ex),
          .retryable = true });
    }
    catch (...) {
      return training_result<std::optional<training_submission_record>>::failure({
        .code = training_errc::provider_transport,
        .message = "training provider lookup callback failed with an unknown exception",
        .retryable = true,
      });
    }
    if (auto* error = result.error_if()) {
      try {
        validate_training_error(*error, training_remote_effect::read_only);
      }
      catch (const std::exception& ex) {
        return training_result<std::optional<training_submission_record>>::failure({
          .code = training_errc::provider_protocol,
          .message = std::string("invalid training Provider lookup error: ") + ex.what(),
        });
      }
    }
    return result;
  }

  [[nodiscard]] training_result<training_provider_snapshot> resume(
    const std::string& provider_job_id, const training_checkpoint& checkpoint,
    const training_context& context) override {
    if (!resume_)
      return training_provider::resume(provider_job_id, checkpoint, context);
    return invoke(
      resume_, training_remote_outcome::uncertain, provider_job_id, checkpoint, context);
  }

private:
  template<typename Callback, typename... Arguments>
  [[nodiscard]] static training_result<training_provider_snapshot> invoke(
    Callback& callback, training_remote_outcome failure_outcome, Arguments&&... arguments) {
    training_result<training_provider_snapshot> result =
      training_result<training_provider_snapshot>::failure({
        .code = training_errc::provider_transport,
        .message = "training provider callback did not respond",
      });
    try {
      result = callback(std::forward<Arguments>(arguments)...);
    }
    catch (const std::exception& ex) {
      return training_result<training_provider_snapshot>::failure(
        { .code = training_errc::provider_transport,
          .message = callback_exception_message(ex),
          .retryable = true,
          .remote_outcome = failure_outcome });
    }
    catch (...) {
      return unknown_callback_failure(failure_outcome);
    }
    if (auto* error = result.error_if()) {
      if (error->remote_outcome == training_remote_outcome::not_applicable &&
          failure_outcome == training_remote_outcome::uncertain) {
        error->remote_outcome = ambiguous_training_error(error->code)
                                  ? training_remote_outcome::uncertain
                                  : training_remote_outcome::not_applied;
      }
      const auto effect = failure_outcome == training_remote_outcome::uncertain
                            ? training_remote_effect::mutation
                            : training_remote_effect::read_only;
      try {
        validate_training_error(*error, effect);
      }
      catch (const std::exception& ex) {
        return training_result<training_provider_snapshot>::failure({
          .code = training_errc::provider_protocol,
          .message = std::string("invalid training Provider error: ") + ex.what(),
          .remote_outcome = failure_outcome,
        });
      }
    }
    return result;
  }

  [[nodiscard]] static training_result<training_provider_snapshot> unknown_callback_failure(
    training_remote_outcome outcome = training_remote_outcome::not_applicable) {
    return training_result<training_provider_snapshot>::failure({
      .code = training_errc::provider_transport,
      .message = "training provider callback failed with an unknown exception",
      .retryable = true,
      .remote_outcome = outcome,
    });
  }

  [[nodiscard]] static std::string callback_exception_message(const std::exception& ex) {
    const std::string message = ex.what();
    return message.empty() ? "training provider callback failed without an error message" : message;
  }

  std::string name_;
  submit_callback submit_;
  inspect_callback inspect_;
  cancel_callback cancel_;
  resume_callback resume_;
  lookup_callback lookup_;
};

using training_observer = std::function<void(const training_job&)>;

struct training_coordinator_options {
  training_observer observer;
  observability::event_sink* event_sink {};
  training_policy policy;
  approval::approval_service* approvals {};
  audit::audit_sink* audit {};
};

class training_coordinator {
public:
  training_coordinator(std::shared_ptr<training_provider> provider, training_job_store& store,
    training_coordinator_options options = {})
      : provider_(std::move(provider)), store_(store), options_(std::move(options)) {
    if (!provider_)
      throw std::invalid_argument("training coordinator requires a provider");
    provider_name_ = provider_->name();
    if (provider_name_.empty())
      throw std::invalid_argument("training coordinator requires a non-empty provider name");
    core::validate_storage_capabilities(store_.capabilities());
  }

  [[nodiscard]] training_result<training_job> submit(
    training_request request, training_context context = {}) {
    try {
      validate_training_request(request);
    }
    catch (const std::exception& ex) {
      return fail({ .code = training_errc::invalid_request, .message = exception_message(ex) });
    }
    if (auto interruption = interrupted(context, "training submission"))
      return fail(*interruption);
    if (auto denied = authorize(training_operation::submit, context, request.target))
      return fail(*denied);
    training_result<std::optional<training_job>> existing_result =
      training_result<std::optional<training_job>>::success(std::nullopt);
    try {
      existing_result = store_.find_by_idempotency_key(
        context.tenant_id, context.workspace_id, request.idempotency_key);
    }
    catch (const std::exception& ex) {
      return persistence_failure(exception_message(ex));
    }
    if (const auto* error = existing_result.error_if())
      return fail(validated_store_error(*error));
    if (const auto& existing = *existing_result.value_if()) {
      try {
        validate_persisted_job(*existing);
      }
      catch (const std::exception& ex) {
        return corrupted_store(exception_message(ex));
      }
      if (existing->tenant_id != context.tenant_id ||
          existing->workspace_id != context.workspace_id ||
          existing->request.idempotency_key != request.idempotency_key) {
        return corrupted_store(
          "training Store returned an idempotency record outside the requested scope");
      }
      if (existing->provider != provider_name_) {
        return fail({ .code = training_errc::invalid_request,
          .message = "training idempotency key belongs to a different provider" });
      }
      if (existing->request.id != request.id ||
          training_request_digest(existing->request) != training_request_digest(request)) {
        return fail({ .code = training_errc::invalid_request,
          .message = "training idempotency key already exists for a different request" });
      }
      return training_result<training_job>::success(*existing);
    }
    training_result<training_provider_snapshot> submitted =
      training_result<training_provider_snapshot>::failure(
        { .code = training_errc::provider_transport,
          .message = "training provider did not respond" });
    try {
      submitted = provider_->submit(request, context);
    }
    catch (const std::exception& ex) {
      return reconciliation_failure({ .code = training_errc::provider_transport,
                                      .message = exception_message(ex),
                                      .retryable = true,
                                      .remote_outcome = training_remote_outcome::uncertain },
        {},
        request.idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::uncertain);
    }
    catch (...) {
      return reconciliation_failure(
        { .code = training_errc::provider_transport,
          .message = "training provider failed with an unknown exception",
          .retryable = true,
          .remote_outcome = training_remote_outcome::uncertain },
        {},
        request.idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::uncertain);
    }
    if (const auto* error = submitted.error_if()) {
      try {
        validate_training_error(*error, training_remote_effect::mutation);
      }
      catch (const std::exception& ex) {
        return reconciliation_failure(
          { .code = training_errc::provider_protocol,
            .message = std::string("invalid Provider error: ") + ex.what(),
            .remote_outcome = training_remote_outcome::uncertain },
          {},
          request.idempotency_key,
          context,
          "training.submit",
          0,
          training_remote_outcome::uncertain);
      }
      if (requires_reconciliation(*error))
        return reconciliation_failure(*error,
          {},
          request.idempotency_key,
          context,
          "training.submit",
          0,
          effective_remote_outcome(*error));
      return fail(*error);
    }
    auto snapshot = *submitted.value_if();
    try {
      validate_provider_snapshot(snapshot);
      if (snapshot.status == training_job_status::succeeded) {
        validate_model_artifact(*snapshot.artifact, &request);
        validate_artifact_origin(*snapshot.artifact, provider_name_, snapshot.provider_job_id);
      }
    }
    catch (const std::exception& ex) {
      return reconciliation_failure(
        { .code = training_errc::provider_protocol, .message = exception_message(ex) },
        snapshot.provider_job_id,
        request.idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::applied);
    }
    const auto provider_job_id = snapshot.provider_job_id;
    const auto idempotency_key = request.idempotency_key;
    training_job job { .id = make_training_id("training-job"),
      .provider = provider_name_,
      .tenant_id = context.tenant_id,
      .workspace_id = context.workspace_id,
      .request = std::move(request),
      .provider_state = std::move(snapshot) };
    const auto proposed = job;
    training_result<training_job_create_result> created =
      training_result<training_job_create_result>::failure(
        { .code = training_errc::persistence_failure,
          .message = "training job was not persisted" });
    try {
      created = store_.create(std::move(job));
    }
    catch (const std::exception& ex) {
      return reconciliation_failure({ .code = training_errc::persistence_failure,
                                      .message = exception_message(ex),
                                      .retryable = true },
        provider_job_id,
        idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::applied);
    }
    if (const auto* error = created.error_if())
      return reconciliation_failure(validated_store_error(*error),
        provider_job_id,
        idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::applied);
    try {
      validate_created_job(proposed, *created.value_if());
    }
    catch (const std::exception& ex) {
      return reconciliation_failure(
        { .code = training_errc::corrupted_state, .message = exception_message(ex) },
        provider_job_id,
        idempotency_key,
        context,
        "training.submit",
        0,
        training_remote_outcome::applied);
    }
    return training_result<training_job>::success(
      persist_and_publish(created.value_if()->job, "submitted"));
  }

  [[nodiscard]] training_result<training_job> refresh(
    const std::string& job_id, training_context context = {}) {
    return update(training_operation::refresh, job_id, context);
  }

  [[nodiscard]] training_result<training_job> reconcile_submit(
    training_request request, training_context context = {}) {
    try {
      validate_training_request(request);
    }
    catch (const std::exception& ex) {
      return fail({ .code = training_errc::invalid_request, .message = exception_message(ex) });
    }
    if (auto interruption = interrupted(context, "training submission reconciliation"))
      return fail(*interruption);
    if (auto denied = authorize(training_operation::reconcile, context, request.target))
      return fail(*denied);

    training_result<std::optional<training_job>> existing_result =
      training_result<std::optional<training_job>>::success(std::nullopt);
    try {
      existing_result = store_.find_by_idempotency_key(
        context.tenant_id, context.workspace_id, request.idempotency_key);
    }
    catch (const std::exception& ex) {
      return persistence_failure(exception_message(ex));
    }
    if (const auto* error = existing_result.error_if())
      return fail(validated_store_error(*error));
    if (const auto& existing = *existing_result.value_if()) {
      try {
        validate_persisted_job(*existing);
      }
      catch (const std::exception& ex) {
        return corrupted_store(exception_message(ex));
      }
      if (existing->tenant_id != context.tenant_id ||
          existing->workspace_id != context.workspace_id ||
          existing->request.idempotency_key != request.idempotency_key) {
        return corrupted_store(
          "training Store returned a reconciliation record outside the requested scope");
      }
      if (existing->provider != provider_name_ || existing->request.id != request.id ||
          training_request_digest(existing->request) != training_request_digest(request))
        return fail({ .code = training_errc::invalid_request,
          .message = "training idempotency key belongs to a different submission" });
      return training_result<training_job>::success(*existing);
    }

    training_result<std::optional<training_submission_record>> lookup =
      training_result<std::optional<training_submission_record>>::failure({
        .code = training_errc::provider_transport,
        .message = "training provider submission lookup did not respond",
      });
    try {
      lookup = provider_->lookup_submission(request.idempotency_key, context);
    }
    catch (const std::exception& ex) {
      return fail({ .code = training_errc::provider_transport,
        .message = exception_message(ex),
        .retryable = true });
    }
    catch (...) {
      return fail({ .code = training_errc::provider_transport,
        .message = "training provider submission lookup failed with an unknown exception",
        .retryable = true });
    }
    if (const auto* error = lookup.error_if()) {
      try {
        validate_training_error(*error, training_remote_effect::read_only);
      }
      catch (const std::exception& ex) {
        return protocol_failure(std::string("invalid Provider lookup failure: ") + ex.what());
      }
      const auto submission_found = error->details.find("submission_found");
      if (error->code == training_errc::provider_protocol &&
          submission_found != error->details.end() && submission_found->is_boolean() &&
          submission_found->get<bool>()) {
        std::string provider_job_id;
        const auto remote_id = error->details.find("provider_job_id");
        if (remote_id != error->details.end() && remote_id->is_string())
          provider_job_id = remote_id->get<std::string>();
        return reconciliation_failure(*error,
          provider_job_id,
          request.idempotency_key,
          context,
          "training.reconcile",
          0,
          training_remote_outcome::applied);
      }
      return fail(*error);
    }
    const auto& found = *lookup.value_if();
    if (!found)
      return fail({ .code = training_errc::not_found,
        .message = "training Provider has no submission for the idempotency key" });
    const auto& submission = *found;
    try {
      validate_training_submission_record(submission);
    }
    catch (const std::exception& ex) {
      return reconciliation_failure(
        { .code = training_errc::provider_protocol, .message = exception_message(ex) },
        submission.snapshot.provider_job_id,
        request.idempotency_key,
        context,
        "training.reconcile",
        0,
        training_remote_outcome::applied);
    }
    const auto expected_digest = training_request_digest(request);
    if (submission.idempotency_key != request.idempotency_key ||
        submission.request_id != request.id || submission.request_digest != expected_digest) {
      return fail({
        .code = training_errc::invalid_request,
        .message =
          "training Provider submission identity does not match the reconciliation request",
      });
    }
    auto snapshot = submission.snapshot;
    try {
      validate_provider_snapshot(snapshot);
      if (snapshot.status == training_job_status::succeeded) {
        validate_model_artifact(*snapshot.artifact, &request);
        validate_artifact_origin(*snapshot.artifact, provider_name_, snapshot.provider_job_id);
      }
    }
    catch (const std::exception& ex) {
      return reconciliation_failure(
        { .code = training_errc::provider_protocol, .message = exception_message(ex) },
        snapshot.provider_job_id,
        request.idempotency_key,
        context,
        "training.reconcile",
        0,
        training_remote_outcome::applied);
    }

    const auto provider_job_id = snapshot.provider_job_id;
    const auto idempotency_key = request.idempotency_key;
    training_job job { .id = make_training_id("training-job"),
      .provider = provider_name_,
      .tenant_id = context.tenant_id,
      .workspace_id = context.workspace_id,
      .request = std::move(request),
      .provider_state = std::move(snapshot) };
    const auto proposed = job;
    training_result<training_job_create_result> created =
      training_result<training_job_create_result>::failure({
        .code = training_errc::persistence_failure,
        .message = "reconciled training job was not persisted",
      });
    try {
      created = store_.create(std::move(job));
    }
    catch (const std::exception& ex) {
      return reconciliation_failure({ .code = training_errc::persistence_failure,
                                      .message = exception_message(ex),
                                      .retryable = true },
        provider_job_id,
        idempotency_key,
        context,
        "training.reconcile",
        0,
        training_remote_outcome::applied);
    }
    if (const auto* error = created.error_if())
      return reconciliation_failure(validated_store_error(*error),
        provider_job_id,
        idempotency_key,
        context,
        "training.reconcile",
        0,
        training_remote_outcome::applied);
    try {
      validate_created_job(proposed, *created.value_if());
    }
    catch (const std::exception& ex) {
      return reconciliation_failure(
        { .code = training_errc::corrupted_state, .message = exception_message(ex) },
        provider_job_id,
        idempotency_key,
        context,
        "training.reconcile",
        0,
        training_remote_outcome::applied);
    }
    return training_result<training_job>::success(
      persist_and_publish(created.value_if()->job, "reconciled"));
  }

  [[nodiscard]] training_result<training_job> cancel(
    const std::string& job_id, training_context context = {}) {
    return update(training_operation::cancel, job_id, context);
  }

  [[nodiscard]] training_result<training_job> resume(
    const std::string& job_id, training_context context = {}) {
    return update(training_operation::resume, job_id, context);
  }

  [[nodiscard]] training_result<std::optional<training_job>> load(
    const std::string& job_id, const training_context& context = {}) const {
    auto loaded = require_job(job_id, context);
    if (const auto* error = loaded.error_if()) {
      if (error->code == training_errc::not_found)
        return training_result<std::optional<training_job>>::success(std::nullopt);
      return training_result<std::optional<training_job>>::failure(*error);
    }
    return training_result<std::optional<training_job>>::success(*loaded.value_if());
  }

private:
  [[nodiscard]] static std::string exception_message(const std::exception& ex) {
    const std::string message = ex.what();
    return message.empty() ? "training operation failed without an error message" : message;
  }

  [[nodiscard]] training_result<training_job> update(
    training_operation operation, const std::string& job_id, const training_context& context) {
    if (auto interruption = interrupted(context, "training operation"))
      return fail(*interruption);
    if (auto denied = authorize(operation, context, job_id))
      return fail(*denied);
    auto loaded = require_job(job_id, context);
    if (const auto* error = loaded.error_if())
      return fail(*error);
    auto job = *loaded.value_if();
    if (terminal(job.provider_state.status)) {
      if (operation == training_operation::refresh)
        return training_result<training_job>::success(std::move(job));
      return fail({ .code = training_errc::invalid_transition,
        .message = "terminal training jobs cannot be cancelled or resumed" });
    }
    training_result<training_provider_snapshot> response =
      training_result<training_provider_snapshot>::failure({
        .code = training_errc::unsupported_operation,
        .message = "unsupported training operation",
      });
    try {
      if (operation == training_operation::refresh)
        response = provider_->inspect(job.provider_state.provider_job_id, context);
      else if (operation == training_operation::cancel)
        response = provider_->cancel(job.provider_state.provider_job_id, context);
      else
        response = resume_provider(job, context);
    }
    catch (const std::exception& ex) {
      if (operation != training_operation::refresh)
        return reconciliation_failure({ .code = training_errc::provider_transport,
                                        .message = exception_message(ex),
                                        .retryable = true,
                                        .remote_outcome = training_remote_outcome::uncertain },
          job.provider_state.provider_job_id,
          job.request.idempotency_key,
          context,
          capability_name(operation),
          job.revision,
          training_remote_outcome::uncertain);
      return fail({ .code = training_errc::provider_transport,
        .message = exception_message(ex),
        .retryable = true });
    }
    catch (...) {
      if (operation != training_operation::refresh)
        return reconciliation_failure(
          { .code = training_errc::provider_transport,
            .message = "training provider failed with an unknown exception",
            .retryable = true,
            .remote_outcome = training_remote_outcome::uncertain },
          job.provider_state.provider_job_id,
          job.request.idempotency_key,
          context,
          capability_name(operation),
          job.revision,
          training_remote_outcome::uncertain);
      return fail({ .code = training_errc::provider_transport,
        .message = "training provider failed with an unknown exception",
        .retryable = true });
    }
    if (const auto* error = response.error_if()) {
      const auto effect = operation == training_operation::refresh
                            ? training_remote_effect::read_only
                            : training_remote_effect::mutation;
      try {
        validate_training_error(*error, effect);
      }
      catch (const std::exception& ex) {
        if (operation != training_operation::refresh)
          return reconciliation_failure(
            { .code = training_errc::provider_protocol,
              .message = std::string("invalid Provider error: ") + ex.what(),
              .remote_outcome = training_remote_outcome::uncertain },
            job.provider_state.provider_job_id,
            job.request.idempotency_key,
            context,
            capability_name(operation),
            job.revision,
            training_remote_outcome::uncertain);
        return protocol_failure(std::string("invalid Provider error: ") + ex.what());
      }
      if (operation != training_operation::refresh && requires_reconciliation(*error))
        return reconciliation_failure(*error,
          job.provider_state.provider_job_id,
          job.request.idempotency_key,
          context,
          capability_name(operation),
          job.revision,
          effective_remote_outcome(*error));
      return fail(*error);
    }
    return apply_snapshot(std::move(job), *response.value_if(), operation, context);
  }

  [[nodiscard]] training_result<training_provider_snapshot> resume_provider(
    const training_job& job, const training_context& context) const {
    if (job.provider_state.status != training_job_status::paused) {
      return training_result<training_provider_snapshot>::failure({
        .code = training_errc::invalid_transition,
        .message = "only paused training jobs may be resumed",
        .remote_outcome = training_remote_outcome::not_applied,
      });
    }
    if (!job.provider_state.latest_checkpoint) {
      return training_result<training_provider_snapshot>::failure({
        .code = training_errc::invalid_request,
        .message = "training resume requires a checkpoint",
        .remote_outcome = training_remote_outcome::not_applied,
      });
    }
    return provider_->resume(
      job.provider_state.provider_job_id, *job.provider_state.latest_checkpoint, context);
  }

  [[nodiscard]] training_result<training_job> require_job(
    const std::string& id, const training_context& context) const {
    training_result<std::optional<training_job>> job =
      training_result<std::optional<training_job>>::success(std::nullopt);
    try {
      job = store_.load(id);
    }
    catch (const std::exception& ex) {
      return persistence_failure(exception_message(ex));
    }
    if (const auto* error = job.error_if())
      return fail(validated_store_error(*error));
    const auto& value = *job.value_if();
    if (!value)
      return fail({ .code = training_errc::not_found, .message = "training job not found: " + id });
    try {
      validate_persisted_job(*value);
    }
    catch (const std::exception& ex) {
      return corrupted_store(exception_message(ex));
    }
    if (value->id != id)
      return corrupted_store("training Store load returned a different job id");
    if (value->tenant_id != context.tenant_id || value->workspace_id != context.workspace_id) {
      return fail({ .code = training_errc::not_found,
        .message = "training job not found in the requested scope: " + id });
    }
    if (value->provider != provider_name_) {
      return fail({ .code = training_errc::invalid_request,
        .message = "training job belongs to a different provider" });
    }
    return training_result<training_job>::success(*value);
  }

  [[nodiscard]] training_result<training_job> apply_snapshot(training_job job,
    training_provider_snapshot snapshot, training_operation operation,
    const training_context& context) {
    const auto provider_job_id = job.provider_state.provider_job_id;
    const auto revision = job.revision;
    const auto after_remote_mutation =
      operation == training_operation::cancel || operation == training_operation::resume;
    const auto local_failure = [&](training_error error) {
      if (!after_remote_mutation)
        return fail(std::move(error));
      return reconciliation_failure(error,
        provider_job_id,
        job.request.idempotency_key,
        context,
        capability_name(operation),
        revision,
        training_remote_outcome::applied);
    };
    try {
      validate_provider_snapshot(snapshot);
    }
    catch (const std::exception& ex) {
      return local_failure(
        { .code = training_errc::provider_protocol, .message = exception_message(ex) });
    }
    if (snapshot.provider_job_id != job.provider_state.provider_job_id)
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training provider changed the provider job id" });
    if (!valid_transition(job.provider_state.status, snapshot.status)) {
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "invalid training job status transition from " +
                   to_string(job.provider_state.status) + " to " + to_string(snapshot.status) });
    }
    if (snapshot.progress < job.provider_state.progress)
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training progress must not move backwards" });
    if (snapshot.current_step < job.provider_state.current_step)
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training current step must not move backwards" });
    if (job.provider_state.total_steps && snapshot.total_steps != job.provider_state.total_steps) {
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training total steps are immutable once declared" });
    }
    if (snapshot.status == training_job_status::succeeded) {
      try {
        validate_model_artifact(*snapshot.artifact, &job.request);
        validate_artifact_origin(*snapshot.artifact, job.provider, snapshot.provider_job_id);
      }
      catch (const std::exception& ex) {
        return local_failure(
          { .code = training_errc::provider_protocol, .message = exception_message(ex) });
      }
    }
    if (!snapshot.latest_checkpoint && job.provider_state.latest_checkpoint)
      snapshot.latest_checkpoint = job.provider_state.latest_checkpoint;
    if (snapshot.latest_checkpoint && job.provider_state.latest_checkpoint &&
        snapshot.latest_checkpoint->step < job.provider_state.latest_checkpoint->step) {
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training checkpoint step must not move backwards" });
    }
    if (snapshot.latest_checkpoint && job.provider_state.latest_checkpoint &&
        snapshot.latest_checkpoint->step == job.provider_state.latest_checkpoint->step &&
        !same_training_checkpoint(
          *snapshot.latest_checkpoint, *job.provider_state.latest_checkpoint)) {
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training checkpoint history is immutable" });
    }
    if (!merge_metrics(job.provider_state.metrics, snapshot.metrics))
      return local_failure({ .code = training_errc::provider_protocol,
        .message = "training metric history is immutable" });
    const auto previous = job;
    job.provider_state = std::move(snapshot);
    const auto expected = job;
    training_result<training_job> updated = training_result<training_job>::failure(
      { .code = training_errc::persistence_failure, .message = "training job was not updated" });
    try {
      updated = store_.update(std::move(job), revision);
    }
    catch (const std::exception& ex) {
      return local_failure({ .code = training_errc::persistence_failure,
        .message = exception_message(ex),
        .retryable = true });
    }
    if (const auto* error = updated.error_if())
      return local_failure(validated_store_error(*error));
    try {
      validate_updated_job(previous, expected, *updated.value_if());
    }
    catch (const std::exception& ex) {
      return local_failure(
        { .code = training_errc::corrupted_state, .message = exception_message(ex) });
    }
    return training_result<training_job>::success(
      persist_and_publish(*updated.value_if(), "state_changed"));
  }

  [[nodiscard]] static std::optional<training_error> interrupted(
    const training_context& context, const char* operation) {
    if (context.cancellation_requested())
      return training_error { .code = training_errc::cancelled,
        .message = std::string(operation) + " cancelled" };
    if (context.deadline_reached())
      return training_error { .code = training_errc::timed_out,
        .message = std::string(operation) + " timed out" };
    return std::nullopt;
  }

  [[nodiscard]] std::optional<training_error> authorize(training_operation operation,
    const training_context& context, const std::string& resource) const {
    const auto evaluation = evaluate_training_policy(operation, options_.policy, context, resource);
    audit_policy(operation, context, resource, evaluation);
    if (evaluation.decision == capability::capability_policy_decision::deny) {
      return training_error { .code = training_errc::authorization_denied,
        .message = evaluation.reason };
    }
    if (evaluation.decision == capability::capability_policy_decision::require_approval) {
      audit_event(operation,
        context,
        resource,
        audit::audit_event_outcome::attempted,
        { { "stage", "approval" }, { "decision", "required" } });
      if (!options_.approvals) {
        audit_event(operation,
          context,
          resource,
          audit::audit_event_outcome::denied,
          { { "stage", "approval" }, { "decision", "missing" } });
        return training_error { .code = training_errc::authorization_denied,
          .message = "training operation requires approval but no approval service is configured" };
      }
      approval::approval_decision decision;
      try {
        decision = options_.approvals->decide({ .id = make_training_id("approval"),
          .summary = evaluation.reason,
          .capabilities = evaluation.capabilities,
          .metadata = evaluation.metadata });
      }
      catch (const std::exception& ex) {
        audit_event(operation,
          context,
          resource,
          audit::audit_event_outcome::denied,
          { { "stage", "approval" }, { "decision", "service_failure" } });
        return training_error { .code = training_errc::authorization_denied,
          .message = std::string("training approval service failed: ") + ex.what() };
      }
      if (decision.kind != approval::approval_decision_kind::approved) {
        audit_event(operation,
          context,
          resource,
          audit::audit_event_outcome::denied,
          { { "stage", "approval" }, { "decision", approval::to_string(decision.kind) } });
        return training_error { .code = training_errc::authorization_denied,
          .message = decision.reason.empty() ? "training approval denied" : decision.reason };
      }
      audit_event(operation,
        context,
        resource,
        audit::audit_event_outcome::approved,
        { { "stage", "approval" }, { "decision", "approved" } });
    }
    return std::nullopt;
  }

  void audit_policy(training_operation operation, const training_context& context,
    const std::string& resource, const capability::capability_policy_result& evaluation) const {
    audit_event(operation,
      context,
      resource,
      evaluation.decision == capability::capability_policy_decision::deny
        ? audit::audit_event_outcome::denied
        : audit::audit_event_outcome::allowed,
      { { "stage", "policy" }, { "decision", capability::to_string(evaluation.decision) } });
  }

  void audit_event(training_operation operation, const training_context& context,
    const std::string& resource, audit::audit_event_outcome outcome,
    std::map<std::string, std::string> attributes = {}) const noexcept {
    if (!options_.audit)
      return;
    attributes.insert_or_assign("resource", resource);
    attributes.insert_or_assign("tenant_id", context.tenant_id);
    attributes.insert_or_assign("workspace_id", context.workspace_id);
    attributes.insert_or_assign("request_id", context.request_id);
    try {
      options_.audit->publish({ .module = "training",
        .name = capability_name(operation),
        .id = context.request_id,
        .trace_id = context.trace_id,
        .subject_id = context.subject_id,
        .outcome = outcome,
        .attributes = std::move(attributes) });
    }
    catch (...) {
    }
  }

  [[nodiscard]] static training_result<training_job> fail(training_error error) {
    return training_result<training_job>::failure(std::move(error));
  }

  [[nodiscard]] static training_error validated_store_error(const training_error& error) {
    try {
      validate_training_error(error, training_remote_effect::read_only);
      return error;
    }
    catch (const std::exception& ex) {
      return {
        .code = training_errc::corrupted_state,
        .message = std::string("invalid training Store error: ") + ex.what(),
      };
    }
  }

  [[nodiscard]] static training_result<training_job> protocol_failure(std::string message) {
    return fail({ .code = training_errc::provider_protocol, .message = std::move(message) });
  }

  [[nodiscard]] static training_result<training_job> persistence_failure(std::string message) {
    return fail({ .code = message.find("revision conflict") != std::string::npos
                            ? training_errc::revision_conflict
                            : training_errc::persistence_failure,
      .message = std::move(message) });
  }

  [[nodiscard]] static training_result<training_job> corrupted_store(std::string message) {
    return fail({ .code = training_errc::corrupted_state, .message = std::move(message) });
  }

  [[nodiscard]] training_result<training_job> reconciliation_failure(
    const training_error& persistence_error, const std::string& provider_job_id,
    const std::string& idempotency_key, const training_context& context,
    std::string operation = "training.submit", std::uint64_t local_revision = 0,
    training_remote_outcome remote_outcome = training_remote_outcome::applied) const {
    const auto uncertain = remote_outcome == training_remote_outcome::uncertain;
    return fail({
      .code = training_errc::reconciliation_required,
      .message = uncertain
        ? "remote training operation may have been applied but no authoritative result was received; reconcile before retrying"
        : "remote training operation succeeded but its resulting state was not committed locally; reconcile before retrying",
      .retryable = false,
      .details = {
        { "provider", provider_name_ },
        { "provider_job_id", provider_job_id },
        { "request_id", context.request_id },
        { "tenant_id", context.tenant_id },
        { "workspace_id", context.workspace_id },
        { "idempotency_key", idempotency_key },
        { "operation", operation },
        { "local_revision", local_revision },
        { "underlying_error_code", to_string(persistence_error.code) },
        { "underlying_error", persistence_error.message },
        { "remote_outcome", to_string(remote_outcome) },
      },
      .metadata = { { "provider", provider_name_ }, { "provider_job_id", provider_job_id },
        { "tenant_id", context.tenant_id }, { "workspace_id", context.workspace_id } },
      .remote_outcome = remote_outcome,
    });
  }

  [[nodiscard]] static training_remote_outcome effective_remote_outcome(
    const training_error& error) noexcept {
    if (error.remote_outcome != training_remote_outcome::not_applicable)
      return error.remote_outcome;
    switch (error.code) {
      case training_errc::provider_transport:
      case training_errc::provider_protocol:
      case training_errc::cancelled:
      case training_errc::timed_out:
      case training_errc::reconciliation_required:
        return training_remote_outcome::uncertain;
      default:
        return training_remote_outcome::not_applied;
    }
  }

  [[nodiscard]] static bool requires_reconciliation(const training_error& error) noexcept {
    const auto outcome = effective_remote_outcome(error);
    return outcome == training_remote_outcome::applied ||
           outcome == training_remote_outcome::uncertain;
  }

  static void validate_artifact_origin(const model_artifact_manifest& artifact,
    const std::string& provider, const std::string& provider_job_id) {
    if (artifact.provider != provider || artifact.provider_job_id != provider_job_id) {
      throw std::invalid_argument("model artifact origin does not match its training provider");
    }
  }

  static void validate_persisted_job(const training_job& job) {
    validate_training_job(job);
    if (job.revision == 0)
      throw std::invalid_argument("training Store returned a job without a persisted revision");
    if (job.updated_at < job.created_at)
      throw std::invalid_argument("training Store returned invalid job timestamps");
  }

  static void validate_created_job(
    const training_job& proposed, const training_job_create_result& result) {
    const auto& created = result.job;
    validate_persisted_job(created);
    if (created.provider != proposed.provider || created.tenant_id != proposed.tenant_id ||
        created.workspace_id != proposed.workspace_id || created.metadata != proposed.metadata ||
        created.provider_state.provider_job_id != proposed.provider_state.provider_job_id) {
      throw std::invalid_argument("training Store create returned a different job identity");
    }
    if (result.inserted) {
      if (created.revision != 1 || created.id != proposed.id ||
          training_request_to_json(created.request) != training_request_to_json(proposed.request) ||
          training_provider_snapshot_to_json(created.provider_state) !=
            training_provider_snapshot_to_json(proposed.provider_state)) {
        throw std::invalid_argument(
          "training Store create did not persist the requested initial revision");
      }
      return;
    }
    if (created.request.id != proposed.request.id ||
        created.request.idempotency_key != proposed.request.idempotency_key ||
        training_request_digest(created.request) != training_request_digest(proposed.request)) {
      throw std::invalid_argument(
        "training Store returned an existing job for a different request");
    }
    if (created.id == proposed.id)
      throw std::invalid_argument(
        "training Store reported an existing idempotency record with the proposed job id");
  }

  static void validate_updated_job(
    const training_job& previous, const training_job& expected, const training_job& updated) {
    validate_persisted_job(updated);
    validate_training_job_update(previous, updated);
    if (previous.revision == (std::numeric_limits<std::uint64_t>::max)() ||
        updated.revision != previous.revision + 1 || updated.id != expected.id ||
        updated.provider != expected.provider || updated.tenant_id != expected.tenant_id ||
        updated.workspace_id != expected.workspace_id ||
        training_request_to_json(updated.request) != training_request_to_json(expected.request) ||
        training_provider_snapshot_to_json(updated.provider_state) !=
          training_provider_snapshot_to_json(expected.provider_state) ||
        updated.metadata != expected.metadata ||
        !same_training_time(updated.created_at, previous.created_at) ||
        updated.updated_at < previous.updated_at) {
      throw std::invalid_argument("training Store update did not persist the requested revision");
    }
  }

  [[nodiscard]] static bool merge_metrics(
    const std::vector<training_metric>& previous, std::vector<training_metric>& current) {
    for (const auto& metric : previous) {
      const auto found = std::find_if(current.begin(), current.end(), [&](const auto& candidate) {
        return candidate.step == metric.step && candidate.name == metric.name;
      });
      if (found == current.end())
        current.push_back(metric);
      else if (!same_training_metric(*found, metric))
        return false;
    }
    std::sort(current.begin(), current.end(), [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.step, lhs.name) < std::tie(rhs.step, rhs.name);
    });
    return true;
  }

  training_job persist_and_publish(training_job job, const char* event_name) const {
    if (options_.observer) {
      (void)observability::invoke_telemetry(
        observability::telemetry_failure_mode::ignore, [&] { options_.observer(job); });
    }
    if (options_.event_sink) {
      (void)observability::invoke_telemetry(observability::telemetry_failure_mode::ignore, [&] {
        options_.event_sink->publish({
          .module = "training",
          .name = event_name,
          .subject_id = job.id,
          .request_id = job.request.id,
          .attributes = {
            { "provider", job.provider },
            { "provider_job_id", job.provider_state.provider_job_id },
            { "target", job.request.target },
            { "objective", to_string(job.request.objective) },
            { "adaptation", to_string(adaptation_kind(job.request.adaptation)) },
            { "status", to_string(job.provider_state.status) },
            { "revision", std::to_string(job.revision) },
          },
        });
      });
    }
    return job;
  }

  std::shared_ptr<training_provider> provider_;
  std::string provider_name_;
  training_job_store& store_;
  training_coordinator_options options_;
};

} // namespace wuwe::agent::training

#endif // WUWE_AGENT_TRAINING_TRAINING_PROVIDER_HPP
