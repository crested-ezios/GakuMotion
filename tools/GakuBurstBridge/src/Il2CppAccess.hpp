#pragma once
#include "NativeProfile.hpp"

namespace gbb {
struct MethodInfo { void* pointer; };
struct Il2CppAccess {
    void* (*domainGet)(){};
    void* (*assemblyOpen)(void*, const char*){};
    void* (*assemblyImage)(void*){};
    void* (*classFind)(void*, const char*, const char*){};
    void* (*classParent)(void*){};
    void* (*classType)(void*){};
    void* (*typeObject)(void*){};
    const MethodInfo* (*methodFind)(void*, const char*, int){};
    void* (*fieldFind)(void*, const char*){};
    void* (*fieldType)(void*){};
    int (*typeCode)(void*){};
    uint32_t (*fieldFlags)(void*){};
    int (*fieldOffset)(void*){};
    void (*fieldRead)(void*, void*, void*){};
    void* (*objectClass)(void*){};
    void* (*unbox)(void*){};
    int (*valueSize)(void*, uint32_t*){};
    void* (*icall)(const char*){};
    // Optional, read-only reporting exports. They are never called by detours
    // and do not relax the required runtime ABI gates.
    void* (*classFields)(void*, void**){};
    const char* (*fieldName)(void*){};
    char* (*typeName)(void*){};
    void (*freeName)(void*){};
    void* (*classFromType)(void*){};
    bool Load() {
        HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
        if (!module) return false;
#define GBB_IMPORT(member, name) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name)); if (!member) return false
        GBB_IMPORT(domainGet, "il2cpp_domain_get");
        GBB_IMPORT(assemblyOpen, "il2cpp_domain_assembly_open");
        GBB_IMPORT(assemblyImage, "il2cpp_assembly_get_image");
        GBB_IMPORT(classFind, "il2cpp_class_from_name");
        GBB_IMPORT(classParent, "il2cpp_class_get_parent");
        GBB_IMPORT(classType, "il2cpp_class_get_type");
        GBB_IMPORT(typeObject, "il2cpp_type_get_object");
        GBB_IMPORT(methodFind, "il2cpp_class_get_method_from_name");
        GBB_IMPORT(fieldFind, "il2cpp_class_get_field_from_name");
        GBB_IMPORT(fieldType, "il2cpp_field_get_type");
        GBB_IMPORT(typeCode, "il2cpp_type_get_type");
        GBB_IMPORT(fieldFlags, "il2cpp_field_get_flags");
        GBB_IMPORT(fieldOffset, "il2cpp_field_get_offset");
        GBB_IMPORT(fieldRead, "il2cpp_field_get_value");
        GBB_IMPORT(objectClass, "il2cpp_object_get_class");
        GBB_IMPORT(unbox, "il2cpp_object_unbox");
        GBB_IMPORT(valueSize, "il2cpp_class_value_size");
        GBB_IMPORT(icall, "il2cpp_resolve_icall");
#undef GBB_IMPORT
        classFields = reinterpret_cast<decltype(classFields)>(GetProcAddress(module, "il2cpp_class_get_fields"));
        fieldName = reinterpret_cast<decltype(fieldName)>(GetProcAddress(module, "il2cpp_field_get_name"));
        typeName = reinterpret_cast<decltype(typeName)>(GetProcAddress(module, "il2cpp_type_get_name"));
        freeName = reinterpret_cast<decltype(freeName)>(GetProcAddress(module, "il2cpp_free"));
        classFromType = reinterpret_cast<decltype(classFromType)>(GetProcAddress(module, "il2cpp_class_from_type"));
        if (!freeName) typeName = nullptr;
        return true;
    }
    void* Class(const char* assembly, const char* space, const char* name) const {
        void* domain = domainGet();
        if (!domain) return nullptr;
        void* a = assemblyOpen(domain, assembly);
        if (!a) return nullptr;
        void* image = assemblyImage(a);
        return image ? classFind(image, space, name) : nullptr;
    }
    void* Field(void* klass, const char* name, int expectedType = -1) const {
        for (; klass; klass = classParent(klass)) {
            void* f = fieldFind(klass, name);
            if (!f) continue;
            if (fieldFlags(f) & 0x10) return nullptr;
            return expectedType < 0 || typeCode(fieldType(f)) == expectedType ? f : nullptr;
        }
        return nullptr;
    }
    bool Offset(void* klass, const char* name, int expected, int type = -1) const {
        void* field = Field(klass, name, type);
        return field && fieldOffset(field) - profile::BoxHeader == expected;
    }
    template<size_t N> bool Layout(void* klass, const profile::Field (&fields)[N]) const {
        for (const auto& field : fields) if (!Offset(klass, field.name, field.offset)) return false;
        return true;
    }
};
} // namespace gbb
