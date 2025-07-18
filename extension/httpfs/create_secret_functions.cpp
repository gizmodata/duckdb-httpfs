#include "create_secret_functions.hpp"
#include "s3fs.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/common/local_file_system.hpp"

namespace duckdb {

void CreateS3SecretFunctions::Register(DatabaseInstance &instance) {
	RegisterCreateSecretFunction(instance, "s3");
	RegisterCreateSecretFunction(instance, "aws");
	RegisterCreateSecretFunction(instance, "r2");
	RegisterCreateSecretFunction(instance, "gcs");
}

static Value MapToStruct(const Value &map) {
	auto children = MapValue::GetChildren(map);

	child_list_t<Value> struct_fields;
	for (const auto &kv_child : children) {
		auto kv_pair = StructValue::GetChildren(kv_child);
		if (kv_pair.size() != 2) {
			throw InvalidInputException("Invalid input passed to refresh_info");
		}

		struct_fields.push_back({kv_pair[0].ToString(), kv_pair[1]});
	}
	return Value::STRUCT(struct_fields);
}
unique_ptr<BaseSecret> CreateS3SecretFunctions::CreateSecretFunctionInternal(ClientContext &context,
                                                                             CreateSecretInput &input) {
	// Set scope to user provided scope or the default
	auto scope = input.scope;
	if (scope.empty()) {
		if (input.type == "s3") {
			scope.push_back("s3://");
			scope.push_back("s3n://");
			scope.push_back("s3a://");
		} else if (input.type == "r2") {
			scope.push_back("r2://");
		} else if (input.type == "gcs") {
			scope.push_back("gcs://");
			scope.push_back("gs://");
		} else if (input.type == "aws") {
			scope.push_back("");
		} else {
			throw InternalException("Unknown secret type found in httpfs extension: '%s'", input.type);
		}
	}

	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->redact_keys = {"secret", "session_token"};

	// for r2 we can set the endpoint using the account id
	if (input.type == "r2" && input.options.find("account_id") != input.options.end()) {
		secret->secret_map["endpoint"] = input.options["account_id"].ToString() + ".r2.cloudflarestorage.com";
		secret->secret_map["url_style"] = "path";
	}

	bool refresh = false;

	// apply any overridden settings
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);

		if (lower_name == "key_id") {
			secret->secret_map["key_id"] = named_param.second;
		} else if (lower_name == "secret") {
			secret->secret_map["secret"] = named_param.second;
		} else if (lower_name == "region") {
			secret->secret_map["region"] = named_param.second.ToString();
		} else if (lower_name == "session_token") {
			secret->secret_map["session_token"] = named_param.second.ToString();
		} else if (lower_name == "endpoint") {
			secret->secret_map["endpoint"] = named_param.second.ToString();
		} else if (lower_name == "url_style") {
			secret->secret_map["url_style"] = named_param.second.ToString();
		} else if (lower_name == "use_ssl") {
			if (named_param.second.type() != LogicalType::BOOLEAN) {
				throw InvalidInputException("Invalid type past to secret option: '%s', found '%s', expected: 'BOOLEAN'",
				                            lower_name, named_param.second.type().ToString());
			}
			secret->secret_map["use_ssl"] = Value::BOOLEAN(named_param.second.GetValue<bool>());
		} else if (lower_name == "kms_key_id") {
			secret->secret_map["kms_key_id"] = named_param.second.ToString();
		} else if (lower_name == "url_compatibility_mode") {
			if (named_param.second.type() != LogicalType::BOOLEAN) {
				throw InvalidInputException("Invalid type past to secret option: '%s', found '%s', expected: 'BOOLEAN'",
				                            lower_name, named_param.second.type().ToString());
			}
			secret->secret_map["url_compatibility_mode"] = Value::BOOLEAN(named_param.second.GetValue<bool>());
		} else if (lower_name == "account_id") {
			continue; // handled already
		} else if (lower_name == "refresh") {
			if (refresh) {
				throw InvalidInputException("Can not set `refresh` and `refresh_info` at the same time");
			}
			refresh = named_param.second.GetValue<string>() == "auto";
			secret->secret_map["refresh"] = Value("auto");
			child_list_t<Value> struct_fields;
			for (const auto &named_param : input.options) {
				auto lower_name = StringUtil::Lower(named_param.first);
				struct_fields.push_back({lower_name, named_param.second});
			}
			secret->secret_map["refresh_info"] = Value::STRUCT(struct_fields);
		} else if (lower_name == "refresh_info") {
			if (refresh) {
				throw InvalidInputException("Can not set `refresh` and `refresh_info` at the same time");
			}
			refresh = true;
			secret->secret_map["refresh_info"] = MapToStruct(named_param.second);
		} else if (lower_name == "requester_pays") {
			if (named_param.second.type() != LogicalType::BOOLEAN) {
				throw InvalidInputException("Invalid type past to secret option: '%s', found '%s', expected: 'BOOLEAN'",
											lower_name, named_param.second.type().ToString());
			}
			secret->secret_map["requester_pays"] = Value::BOOLEAN(named_param.second.GetValue<bool>());
		} else {
			throw InvalidInputException("Unknown named parameter passed to CreateSecretFunctionInternal: " +
			                            lower_name);
		}
	}

	return std::move(secret);
}

CreateSecretInput CreateS3SecretFunctions::GenerateRefreshSecretInfo(const SecretEntry &secret_entry,
                                                                     Value &refresh_info) {
	const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry.secret);

	CreateSecretInput result;
	result.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	result.persist_type = SecretPersistType::TEMPORARY;

	result.type = kv_secret.GetType();
	result.name = kv_secret.GetName();
	result.provider = kv_secret.GetProvider();
	result.storage_type = secret_entry.storage_mode;
	result.scope = kv_secret.GetScope();

	auto result_child_count = StructType::GetChildCount(refresh_info.type());
	auto refresh_info_children = StructValue::GetChildren(refresh_info);
	D_ASSERT(refresh_info_children.size() == result_child_count);
	for (idx_t i = 0; i < result_child_count; i++) {
		auto &key = StructType::GetChildName(refresh_info.type(), i);
		auto &value = refresh_info_children[i];
		result.options[key] = value;
	}

	return result;
}

//! Function that will automatically try to refresh a secret
bool CreateS3SecretFunctions::TryRefreshS3Secret(ClientContext &context, const SecretEntry &secret_to_refresh) {
	const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_to_refresh.secret);

	Value refresh_info;
	if (!kv_secret.TryGetValue("refresh_info", refresh_info)) {
		return false;
	}
	auto &secret_manager = context.db->GetSecretManager();
	auto refresh_input = GenerateRefreshSecretInfo(secret_to_refresh, refresh_info);

	// TODO: change SecretManager API to avoid requiring catching this exception
	try {
		auto res = secret_manager.CreateSecret(context, refresh_input);
		auto &new_secret = dynamic_cast<const KeyValueSecret &>(*res->secret);
		DUCKDB_LOG_INFO(context, "Successfully refreshed secret: %s, new key_id: %s",
		                secret_to_refresh.secret->GetName(), new_secret.TryGetValue("key_id").ToString());
		return true;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		string new_message = StringUtil::Format("Exception thrown while trying to refresh secret %s. To fix this, "
		                                        "please recreate or remove the secret and try again. Error: '%s'",
		                                        secret_to_refresh.secret->GetName(), error.Message());
		throw Exception(error.Type(), new_message);
	}
}

unique_ptr<BaseSecret> CreateS3SecretFunctions::CreateS3SecretFromConfig(ClientContext &context,
                                                                         CreateSecretInput &input) {
	return CreateSecretFunctionInternal(context, input);
}

void CreateS3SecretFunctions::SetBaseNamedParams(CreateSecretFunction &function, string &type) {
	function.named_parameters["key_id"] = LogicalType::VARCHAR;
	function.named_parameters["secret"] = LogicalType::VARCHAR;
	function.named_parameters["region"] = LogicalType::VARCHAR;
	function.named_parameters["session_token"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
	function.named_parameters["url_style"] = LogicalType::VARCHAR;
	function.named_parameters["use_ssl"] = LogicalType::BOOLEAN;
	function.named_parameters["kms_key_id"] = LogicalType::VARCHAR;
	function.named_parameters["url_compatibility_mode"] = LogicalType::BOOLEAN;
    function.named_parameters["requester_pays"] = LogicalType::BOOLEAN;

	// Whether a secret refresh attempt should be made when the secret appears to be incorrect
	function.named_parameters["refresh"] = LogicalType::VARCHAR;

	// Refresh Modes
	// - auto
	// - disabled
	// - on_error
	// - on_timeout

	// - on_use: every time a secret is used, it will refresh.

	// Debugging/testing option: it allows specifying how the secret will be refreshed using a manually specfied MAP
	function.named_parameters["refresh_info"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	if (type == "r2") {
		function.named_parameters["account_id"] = LogicalType::VARCHAR;
	}
}

void CreateS3SecretFunctions::RegisterCreateSecretFunction(DatabaseInstance &instance, string type) {
	// Register the new type
	SecretType secret_type;
	secret_type.name = type;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "httpfs";

	ExtensionUtil::RegisterSecretType(instance, secret_type);

	CreateSecretFunction from_empty_config_fun2 = {type, "config", CreateS3SecretFromConfig};
	SetBaseNamedParams(from_empty_config_fun2, type);
	ExtensionUtil::RegisterFunction(instance, from_empty_config_fun2);
}

void CreateBearerTokenFunctions::Register(DatabaseInstance &instance) {
	// HuggingFace secret
	SecretType secret_type_hf;
	secret_type_hf.name = HUGGINGFACE_TYPE;
	secret_type_hf.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type_hf.default_provider = "config";
	secret_type_hf.extension = "httpfs";
	ExtensionUtil::RegisterSecretType(instance, secret_type_hf);

	// Huggingface config provider
	CreateSecretFunction hf_config_fun = {HUGGINGFACE_TYPE, "config", CreateBearerSecretFromConfig};
	hf_config_fun.named_parameters["token"] = LogicalType::VARCHAR;
	ExtensionUtil::RegisterFunction(instance, hf_config_fun);

	// Huggingface credential_chain provider
	CreateSecretFunction hf_cred_fun = {HUGGINGFACE_TYPE, "credential_chain",
	                                    CreateHuggingFaceSecretFromCredentialChain};
	ExtensionUtil::RegisterFunction(instance, hf_cred_fun);
}

unique_ptr<BaseSecret> CreateBearerTokenFunctions::CreateSecretFunctionInternal(ClientContext &context,
                                                                                CreateSecretInput &input,
                                                                                const string &token) {
	// Set scope to user provided scope or the default
	auto scope = input.scope;
	if (scope.empty()) {
		if (input.type == HUGGINGFACE_TYPE) {
			scope.push_back("hf://");
		} else {
			throw InternalException("Unknown secret type found in httpfs extension: '%s'", input.type);
		}
	}
	auto return_value = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	//! Set key value map
	return_value->secret_map["token"] = token;

	//! Set redact keys
	return_value->redact_keys = {"token"};

	return std::move(return_value);
}

unique_ptr<BaseSecret> CreateBearerTokenFunctions::CreateBearerSecretFromConfig(ClientContext &context,
                                                                                CreateSecretInput &input) {
	string token;

	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);
		if (lower_name == "token") {
			token = named_param.second.ToString();
		}
	}

	return CreateSecretFunctionInternal(context, input, token);
}

static string TryReadTokenFile(const string &token_path, const string error_source_message,
                               bool fail_on_exception = true) {
	try {
		LocalFileSystem fs;
		auto handle = fs.OpenFile(token_path, {FileOpenFlags::FILE_FLAGS_READ});
		return handle->ReadLine();
	} catch (std::exception &ex) {
		if (!fail_on_exception) {
			return "";
		}
		ErrorData error(ex);
		throw IOException("Failed to read token path '%s'%s. (error: %s)", token_path, error_source_message,
		                  error.RawMessage());
	}
}

unique_ptr<BaseSecret>
CreateBearerTokenFunctions::CreateHuggingFaceSecretFromCredentialChain(ClientContext &context,
                                                                       CreateSecretInput &input) {
	// Step 1: Try the ENV variable HF_TOKEN
	const char *hf_token_env = std::getenv("HF_TOKEN");
	if (hf_token_env) {
		return CreateSecretFunctionInternal(context, input, hf_token_env);
	}
	// Step 2: Try the ENV variable HF_TOKEN_PATH
	const char *hf_token_path_env = std::getenv("HF_TOKEN_PATH");
	if (hf_token_path_env) {
		auto token = TryReadTokenFile(hf_token_path_env, " fetched from HF_TOKEN_PATH env variable");
		return CreateSecretFunctionInternal(context, input, token);
	}

	// Step 3: Try the path $HF_HOME/token
	const char *hf_home_env = std::getenv("HF_HOME");
	if (hf_home_env) {
		auto token_path = LocalFileSystem().JoinPath(hf_home_env, "token");
		auto token = TryReadTokenFile(token_path, " constructed using the HF_HOME variable: '$HF_HOME/token'");
		return CreateSecretFunctionInternal(context, input, token);
	}

	// Step 4: Check the default path
	auto token = TryReadTokenFile("~/.cache/huggingface/token", "", false);
	return CreateSecretFunctionInternal(context, input, token);
}
} // namespace duckdb
