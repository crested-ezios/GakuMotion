#pragma once

#include "GkShoeMeshTypes.hpp"

namespace ShoeMeshProbe
{
	struct Api
	{
		void* (*methods)(void*, void**) = nullptr;
		const char* (*methodName)(Method) = nullptr;
		uint32_t (*paramCount)(Method) = nullptr;
		void* (*paramType)(Method, uint32_t) = nullptr;
		void* (*returnType)(Method) = nullptr;
		uint32_t (*flags)(Method, uint32_t*) = nullptr;
		char* (*typeName)(void*) = nullptr;
		void (*freeMemory)(void*) = nullptr;
		void* (*classType)(void*) = nullptr;
		void* (*typeObject)(void*) = nullptr;
		void* (*objectNew)(void*) = nullptr;
		void* (*objectClass)(void*) = nullptr;
		uintptr_t (*arrayLength)(void*) = nullptr;
		size_t (*arrayHeaderSize)() = nullptr;
		uint32_t (*arrayElementSize)(void*) = nullptr;
		Handle (*handleNew)(void*, bool) = nullptr;
		void* (*handleTarget)(Handle) = nullptr;
		void (*handleFree)(Handle) = nullptr;
		int32_t (*stringLength)(void*) = nullptr;
		const wchar_t* (*stringChars)(void*) = nullptr;
		void (*formatException)(void*, char*, int) = nullptr;
		int32_t (*valueSize)(void*, uint32_t*) = nullptr;
	};

	inline Api api;
	inline Json apiEvidence = Object();
	inline std::string configDirectory;
	inline DWORD resolveThread = 0;
	inline bool available = false;
	inline bool rendererAvailable = false;
	inline uint32_t lastSeq = 0;
	inline std::chrono::steady_clock::time_point nextPoll{};
	inline Json officialCachedRequest;
	inline std::string officialCachedPath;
	inline std::chrono::steady_clock::time_point officialNextPoll{};

	inline void* vectorClass = nullptr;
	inline void* matrixClass = nullptr;
	inline void* meshClass = nullptr;
	inline void* rendererClass = nullptr;
	inline void* vectorFields[3]{};
	inline void* matrixFields[16]{};
	inline Method getComponents = nullptr, getTransform = nullptr, getName = nullptr, getBones = nullptr;
	inline Method getSharedMesh = nullptr, getVertexCount = nullptr, getReadable = nullptr, meshCtor = nullptr;
	inline Method bakeMesh = nullptr, getVertices = nullptr, destroyObject = nullptr, arrayGetValue = nullptr;
	inline Method getLocalToWorld = nullptr, getWorldToLocal = nullptr;
	inline Method getEnabled = nullptr, getGameObject = nullptr, getActive = nullptr, getLossyScale = nullptr,
	              getWorldPosition = nullptr;
	inline Handle rendererTypeHandle = nullptr;

	// Pin all captured managed objects: even a moving collector cannot invalidate
	// a pointer while a subsequent runtime_invoke allocates a boxed value.
	struct Pin
	{
		Handle id = nullptr;

		explicit Pin(void* object = nullptr)
		{
			if (object && api.handleNew && TraceFirst("void* il2cpp_gchandle_new(void*, bool pinned=true)"))id = api.
				handleNew(object, true);
		}

		void Close()
		{
			if (id && api.handleFree)
			{
				TraceFirst("void il2cpp_gchandle_free(void*)");
				api.handleFree(id);
				id = nullptr;
			}
		}

		~Pin() { Close(); }
		Pin(const Pin&) = delete;
		Pin& operator=(const Pin&) = delete;

		void* Get() const
		{
			return id && api.handleTarget && TraceFirst("void* il2cpp_gchandle_get_target(void*)")
				       ? api.handleTarget(id)
				       : nullptr;
		}
	};

	inline std::string TypeName(void* type)
	{
		if (!type || !api.typeName || !api.freeMemory)return {};
		char* s = api.typeName(type);
		std::string name = s ? s : "";
		if (s)api.freeMemory(s);
		return name;
	}

	inline std::string ManagedString(void* object)
	{
		if (!object || !api.stringLength || !api.stringChars)return {};
		Pin pin(object);
		if (!pin.Get())return {};
		if (!TraceFirst("int32_t il2cpp_string_length(void*)"))return {};
		const int n = api.stringLength(pin.Get());
		if (n < 0 || n > 2048)return "<name length outside limit>";
		if (!TraceFirst("const wchar_t* il2cpp_string_chars(void*)"))return {};
		const wchar_t* chars = api.stringChars(pin.Get());
		if (!chars)return {};
		int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, n, nullptr, 0, nullptr, nullptr);
		if (bytes <= 0)return n ? "<invalid UTF-16>" : "";
		std::string s(static_cast<size_t>(bytes), '\0');
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, n, &s[0], bytes, nullptr, nullptr);
		return s;
	}

	inline std::string ExceptionText(void* exception)
	{
		if (!exception)return {};
		Pin pin(exception);
		char message[1024]{};
		if (api.formatException && pin.Get() && TraceFirst("void il2cpp_format_exception(void*, char*, int)"))api.
			formatException(pin.Get(), message, sizeof(message));
		return message[0] ? std::string(message) : "managed exception (formatter unavailable)";
	}

	inline void* Invoke(Method method, void* instance, void** args, std::string& error)
	{
		if (!method || !g_safeBones.invoke)
		{
			error = "required method/runtime_invoke unavailable";
			return nullptr;
		}
		if (!TraceFirst(Signature(method)))
		{
			error = "trace persistence failed; managed call not attempted";
			return nullptr;
		}
		void* exception = nullptr;
		void* result = g_safeBones.invoke(method, instance, args, &exception);
		if (exception)
		{
			error = ExceptionText(exception);
			return nullptr;
		}
		return result;
	}

	inline Method Match(void* klass, const char* name, const char* result,
	                    std::initializer_list<const char*> params, bool isStatic)
	{
		Json evidence = Object();
		Set(evidence, "name", Text(name));
		Set(evidence, "expected_return", Text(result));
		Set(evidence, "expected_static", Flag(isStatic));
		Json expected = Array();
		for (auto p : params)Push(expected, Text(p));
		Set(evidence, "expected_parameters", std::move(expected));
		Json observed = Array();
		Method selected = nullptr;
		int matches = 0;
		if (klass && api.methods && api.methodName && api.paramCount && api.paramType && api.returnType && api.flags)
		{
			void* iter = nullptr;
			while (Method method = api.methods(klass, &iter))
			{
				const char* methodName = api.methodName(method);
				if (!methodName || std::strcmp(methodName, name) != 0)continue;
				uint32_t impl = 0;
				bool actualStatic = (api.flags(method, &impl) & 0x10) != 0;
				const std::string ret = TypeName(api.returnType(method));
				const uint32_t count = api.paramCount(method);
				Json signature = Object();
				Set(signature, "return", Text(ret));
				Set(signature, "static", Flag(actualStatic));
				Json actual = Array();
				bool same = count == params.size() && ret == result && actualStatic == isStatic;
				auto wanted = params.begin();
				for (uint32_t i = 0; i < count; ++i)
				{
					const auto p = TypeName(api.paramType(method, i));
					Push(actual, Text(p));
					if (i < params.size())
					{
						same = same && p == *wanted;
						++wanted;
					}
				}
				Set(signature, "parameters", std::move(actual));
				Set(signature, "matches", Flag(same));
				Push(observed, std::move(signature));
				if (same)
				{
					selected = method;
					++matches;
				}
			}
		}
		Set(evidence, "overloads", std::move(observed));
		Set(evidence, "exact_matches", Number(matches));
		// Duplicate metadata entries are ambiguous; do not select the first pointer.
		if (matches != 1)selected = nullptr;
		Set(evidence, "resolved", Flag(selected != nullptr));
		if (selected)
		{
			std::string signature = isStatic ? "static " : "";
			signature += result;
			signature += ' ';
			signature += name;
			signature += '(';
			bool first = true;
			for (auto p : params)
			{
				if (!first)signature += ", ";
				signature += p;
				first = false;
			}
			signature += ')';
			methodSignatures.push_back({selected, std::move(signature)});
		}
		Push(apiEvidence.objectMembers.back().second, std::move(evidence));
		return selected;
	}

	inline bool Fields(void* klass, const std::vector<std::string>& names, void** fields, Json& evidence)
	{
		if (!klass || !g_faceRuntime.findField || !g_faceRuntime.fieldType || !g_faceRuntime.fieldFlags ||
			!g_faceRuntime.typeCode || !g_faceRuntime.readField || !il2cpp_field_get_offset || !api.valueSize)
			return false;
		uint32_t alignment = 0;
		const int size = api.valueSize(klass, &alignment);
		Set(evidence, "value_size", Number(size));
		Set(evidence, "alignment", Number(alignment));
		Json metadata = Array();
		bool good = size == static_cast<int>(4 * names.size());
		std::vector<int> offsets;
		for (size_t i = 0; i < names.size(); ++i)
		{
			fields[i] = g_faceRuntime.findField(klass, names[i].c_str());
			void* type = fields[i] ? g_faceRuntime.fieldType(fields[i]) : nullptr;
			const int offset = fields[i] ? il2cpp_field_get_offset(fields[i]) : -1;
			const bool valid = type && g_faceRuntime.typeCode(type) == 0x0c && !(g_faceRuntime.fieldFlags(fields[i]) &
				0x10) && offset >= 0;
			Json f = Object();
			Set(f, "name", Text(names[i]));
			Set(f, "offset", Number(offset));
			Set(f, "float_instance_field", Flag(valid));
			Push(metadata, std::move(f));
			good = good && valid;
			offsets.push_back(offset);
		}
		std::sort(offsets.begin(), offsets.end());
		for (size_t i = 1; i < offsets.size(); ++i)good = good && offsets[i] == offsets[0] + static_cast<int>(4 * i);
		Set(evidence, "fields", std::move(metadata));
		Set(evidence, "validated", Flag(good));
		return good;
	}

	inline void Resolve(HMODULE assembly, const std::string& root)
	{
		if (probeFaulted)return; // Sticky for the lifetime of this loaded probe.
		configDirectory = root;
		resolveThread = GetCurrentThreadId();
		available = false;
		rendererAvailable = false;
		nextPoll = {};
		lastSeq = 0;
		traceActive = false;
		methodSignatures.clear();
		if (rendererTypeHandle && api.handleFree)api.handleFree(rendererTypeHandle);
		rendererTypeHandle = 0;
		api = {};
		apiEvidence = Object();
		Json exports = Object();
#define SHOE_EXPORT(member,symbol) api.member=reinterpret_cast<decltype(api.member)>(GetProcAddress(assembly,symbol)); Set(exports,symbol,Flag(api.member!=nullptr))
		SHOE_EXPORT(methods, "il2cpp_class_get_methods");
		SHOE_EXPORT(methodName, "il2cpp_method_get_name");
		SHOE_EXPORT(paramCount, "il2cpp_method_get_param_count");
		SHOE_EXPORT(paramType, "il2cpp_method_get_param");
		SHOE_EXPORT(returnType, "il2cpp_method_get_return_type");
		SHOE_EXPORT(flags, "il2cpp_method_get_flags");
		SHOE_EXPORT(typeName, "il2cpp_type_get_name");
		SHOE_EXPORT(freeMemory, "il2cpp_free");
		SHOE_EXPORT(classType, "il2cpp_class_get_type");
		SHOE_EXPORT(typeObject, "il2cpp_type_get_object");
		SHOE_EXPORT(objectNew, "il2cpp_object_new");
		SHOE_EXPORT(objectClass, "il2cpp_object_get_class");
		SHOE_EXPORT(arrayLength, "il2cpp_array_length");
		SHOE_EXPORT(arrayHeaderSize, "il2cpp_array_object_header_size");
		SHOE_EXPORT(arrayElementSize, "il2cpp_array_element_size");
		SHOE_EXPORT(handleNew, "il2cpp_gchandle_new");
		SHOE_EXPORT(handleTarget, "il2cpp_gchandle_get_target");
		SHOE_EXPORT(handleFree, "il2cpp_gchandle_free");
		SHOE_EXPORT(stringLength, "il2cpp_string_length");
		SHOE_EXPORT(stringChars, "il2cpp_string_chars");
		SHOE_EXPORT(formatException, "il2cpp_format_exception");
		SHOE_EXPORT(valueSize, "il2cpp_class_value_size");
#undef SHOE_EXPORT
		Set(apiEvidence, "exports", std::move(exports));
		Set(apiEvidence, "gchandle_storage_bytes", Number(sizeof(Handle)));
#if defined(_MSC_VER)
		Set(apiEvidence, "native_seh_guard", Flag(true));
#else
		Set(apiEvidence, "native_seh_guard", Flag(false));
#endif
		// Store method evidence as the final member while Match appends to it.
		Set(apiEvidence, "methods", Array());
		void* component = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
		void* object = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
		void* transform = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
		void* rendererBase = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
		void* gameObject = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
		rendererClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "SkinnedMeshRenderer");
		meshClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh");
		matrixClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4");
		vectorClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Vector3");
		void* array = FindClass("mscorlib.dll", "System", "Array");
		if (!array)array = FindClass("System.Private.CoreLib.dll", "System", "Array");
		getComponents = Match(component, "GetComponentsInChildren", "UnityEngine.Component[]",
		                      {"System.Type", "System.Boolean"}, false);
		getTransform = Match(component, "get_transform", "UnityEngine.Transform", {}, false);
		getGameObject = Match(component, "get_gameObject", "UnityEngine.GameObject", {}, false);
		getEnabled = Match(rendererBase, "get_enabled", "System.Boolean", {}, false);
		getActive = Match(gameObject, "get_activeInHierarchy", "System.Boolean", {}, false);
		getName = Match(object, "get_name", "System.String", {}, false);
		getBones = Match(rendererClass, "get_bones", "UnityEngine.Transform[]", {}, false);
		getSharedMesh = Match(rendererClass, "get_sharedMesh", "UnityEngine.Mesh", {}, false);
		bakeMesh = Match(rendererClass, "BakeMesh", "System.Void", {"UnityEngine.Mesh", "System.Boolean"}, false);
		getVertexCount = Match(meshClass, "get_vertexCount", "System.Int32", {}, false);
		getReadable = Match(meshClass, "get_isReadable", "System.Boolean", {}, false);
		getVertices = Match(meshClass, "get_vertices", "UnityEngine.Vector3[]", {}, false);
		meshCtor = Match(meshClass, ".ctor", "System.Void", {}, false);
		destroyObject = Match(object, "Destroy", "System.Void", {"UnityEngine.Object"}, true);
		getLocalToWorld = Match(transform, "get_localToWorldMatrix", "UnityEngine.Matrix4x4", {}, false);
		getWorldToLocal = Match(transform, "get_worldToLocalMatrix", "UnityEngine.Matrix4x4", {}, false);
		getLossyScale = Match(transform, "get_lossyScale", "UnityEngine.Vector3", {}, false);
		getWorldPosition = Match(transform, "get_position", "UnityEngine.Vector3", {}, false);
		arrayGetValue = Match(array, "GetValue", "System.Object", {"System.Int32"}, false);
		Json vectorMetadata = Object(), matrixMetadata = Object();
		const bool vectorFieldsOk = Fields(vectorClass, {"x", "y", "z"}, vectorFields, vectorMetadata);
		std::vector<std::string> matrixNames;
		for (int r = 0; r < 4; ++r)for (int c = 0; c < 4; ++c)matrixNames.push_back(
			"m" + std::to_string(r) + std::to_string(c));
		const bool matrixFieldsOk = Fields(matrixClass, matrixNames, matrixFields, matrixMetadata);
		Set(apiEvidence, "vector3", std::move(vectorMetadata));
		Set(apiEvidence, "matrix4x4", std::move(matrixMetadata));
		available = assembly && api.handleNew && api.handleTarget && api.handleFree && api.objectNew && api.objectClass
			&& api.classType && api.typeObject && api.stringLength && api.stringChars &&
			api.arrayLength && g_safeBones.invoke && g_safeBones.unbox && getComponents && getTransform && getName &&
			getBones &&
			getSharedMesh && getVertexCount && getReadable && meshCtor && bakeMesh && getVertices && destroyObject &&
			arrayGetValue &&
			getLocalToWorld && getWorldToLocal && getEnabled && getGameObject && getActive && getLossyScale &&
			getWorldPosition && vectorFieldsOk && matrixFieldsOk;
		rendererAvailable = assembly && api.handleNew && api.handleTarget && api.handleFree && api.objectClass && api.
			classType && api.typeObject && api.stringLength && api.stringChars &&
			api.arrayLength && g_safeBones.invoke && g_safeBones.unbox && getComponents && getName && getBones &&
			getSharedMesh && getVertexCount && getReadable && arrayGetValue &&
			getLocalToWorld && getWorldToLocal && getEnabled && getGameObject && getActive && getWorldPosition &&
			vectorFieldsOk && matrixFieldsOk && rendererClass;
		// Creating a managed System.Type and its handle belongs to renderers, never
		// Resolve or the isolated actor-handle stage.
		Set(apiEvidence, "handle_ready", Flag(api.handleNew && api.handleTarget && api.handleFree));
		Set(apiEvidence, "renderers_ready", Flag(rendererAvailable));
		Set(apiEvidence, "ready", Flag(available));
		Set(apiEvidence, "array_access", Text("System.Array.GetValue(Int32), no array memory offsets used"));
		if (api.arrayHeaderSize)Set(apiEvidence, "runtime_array_header_bytes",
		                            Number(static_cast<double>(api.arrayHeaderSize())));
	}

	inline std::string Name(void* object)
	{
		if (!object)return {};
		std::string error;
		return ManagedString(Invoke(getName, object, nullptr, error));
	}

	inline void* ObjectClass(void* object)
	{
		return object && api.objectClass && TraceFirst("Il2CppClass* il2cpp_object_get_class(void*)")
			       ? api.objectClass(object)
			       : nullptr;
	}

	inline uintptr_t ArrayLength(void* object)
	{
		return object && api.arrayLength && TraceFirst("uintptr_t il2cpp_array_length(void*)")
			       ? api.arrayLength(object)
			       : 0;
	}

	inline bool Primitive(Method method, void* object, int code, void* destination, std::string& error)
	{
		Pin boxed(Invoke(method, object, nullptr, error));
		if (!boxed.Get())return false;
		void* klass = ObjectClass(boxed.Get());
		void* type = klass && api.classType && TraceFirst("Il2CppType* il2cpp_class_get_type(Il2CppClass*)")
			             ? api.classType(klass)
			             : nullptr;
		if (!type || !g_faceRuntime.typeCode || g_faceRuntime.typeCode(type) != code)
		{
			error = "boxed primitive return type mismatch";
			return false;
		}
		const void* payload = TraceFirst("void* il2cpp_object_unbox(void*)") ? g_safeBones.unbox(boxed.Get()) : nullptr;
		if (!payload)
		{
			error = "boxed primitive unbox failed";
			return false;
		}
		std::memcpy(destination, payload, code == 0x02 ? sizeof(bool) : sizeof(int32_t));
		return true;
	}

	inline void* Element(void* array, int index, std::string& error)
	{
		void* args[] = {&index};
		return Invoke(arrayGetValue, array, args, error);
	}

	inline Json ArrayInfo(void* array)
	{
		Json j = Object();
		Set(j, "length", Number(static_cast<double>(ArrayLength(array))));
		if (api.arrayElementSize && TraceFirst("uint32_t il2cpp_array_element_size(Il2CppClass*)"))Set(
			j, "runtime_element_bytes", Number(api.arrayElementSize(ObjectClass(array))));
		return j;
	}
} // namespace ShoeMeshProbe
