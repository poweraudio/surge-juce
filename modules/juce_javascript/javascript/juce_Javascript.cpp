/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

//==============================================================================
// On Linux int64 and int64_t don't resolve to the same type and don't have C style casts between
// each other, hence we need a two-step conversion.
template <typename T>
static int64_t fromJuceInt64 (const T& convertible) { return (int64_t) (int64) convertible; }

template <typename T>
static int64_t toJuceInt64   (const T& convertible) { return (int64) (int64_t) convertible; }

//==============================================================================
namespace qjs = detail::choc::javascript::quickjs;

using VarOrError = std::variant<var, String>;

static var discardError (VarOrError variant)
{
    const auto* v = std::get_if<var> (&variant);
    return v != nullptr ? *v : var::undefined();
}

static VarOrError quickJSToJuce (const qjs::QuickJSContext::ValuePtr& ptr);

static std::vector<var> quickJSToJuce (Span<qjs::JSValueConst> args, qjs::JSContext* ctx)
{
    std::vector<var> argList;
    argList.reserve (args.size());

    for (const auto& arg : args)
        argList.push_back (discardError (quickJSToJuce ({ qjs::JS_DupValue (ctx, arg), ctx })));

    return argList;
}

static qjs::JSValue juceToQuickJs (const var& v, qjs::JSContext* ctx)
{
    using namespace qjs;

    if (v.isVoid())
        return JS_NULL;

    if (v.isUndefined())
        return JS_UNDEFINED;

    if (v.isInt())
        return JS_NewInt32   (ctx, static_cast<int> (v));

    if (v.isInt64())
        return JS_NewInt64   (ctx, static_cast<int64> (v));

    if (v.isDouble())
        return JS_NewFloat64 (ctx, static_cast<double>  (v));

    if (v.isBool())
        return JS_NewBool    (ctx, static_cast<bool>    (v));

    if (v.isString())
    {
        const String x = v;
        return JS_NewStringLen (ctx, x.toRawUTF8(), x.getNumBytesAsUTF8());
    }

    if (auto fn = v.getNativeFunction())
    {
        using Fn = var::NativeFunction;
        static constexpr auto size = sizeof (fn);

        const auto cb = [] (JSContext* localContext,
                            JSValueConst thisVal,
                            int argc,
                            JSValueConst* argv,
                            int,
                            JSValue* funcData) -> JSValue
        {
            if (funcData == nullptr)
            {
                jassertfalse;
                return {};
            }

            size_t bufferSize{};
            void* buffer = qjs::JS_GetArrayBuffer (localContext, &bufferSize, *funcData);

            if (buffer == nullptr || bufferSize != size)
            {
                jassertfalse;
                return {};
            }

            const auto thisConverted = discardError (quickJSToJuce ({ qjs::JS_DupValue (localContext, thisVal), localContext }));
            const auto argsConverted = quickJSToJuce ({ argv, (size_t) argc }, localContext);
            const var::NativeFunctionArgs args { thisConverted, argsConverted.data(), (int) argsConverted.size() };

            const auto resultVar = (*static_cast<Fn*> (buffer)) (args);

            return juceToQuickJs (resultVar, localContext);
        };

        const auto free = [] (JSRuntime*, void*, void* buffer)
        {
            auto* localFn = static_cast<Fn*> (buffer);
            localFn->~Fn();
            delete[] static_cast<uint8_t*> (buffer);
        };

        std::unique_ptr<uint8_t[]> storage { new uint8_t[size] };
        new (storage.get()) Fn { std::move (fn) };

        qjs::QuickJSContext::ValuePtr callbackAsData { qjs::JS_NewArrayBuffer (ctx,
                                                                               storage.release(),
                                                                               size,
                                                                               free,
                                                                               nullptr,
                                                                               false),
                                                       ctx };
        return JS_NewCFunctionData (ctx, cb, 0, 0, 1, &callbackAsData.value);
    }

    if (auto* array = v.getArray())
    {
        auto result = JS_NewArray (ctx);

        for (const auto [index, value] : enumerate (*array, uint32_t{}))
            JS_SetPropertyUint32 (ctx, result, index, juceToQuickJs (value, ctx));

        return result;
    }

    if (auto* obj = v.getDynamicObject())
    {
        auto result = JS_NewObject (ctx);

        for (const auto& pair : obj->getProperties())
        {
            const auto name = pair.name.toString();
            JS_SetPropertyStr (ctx, result, name.toRawUTF8(), juceToQuickJs (pair.value, ctx));
        }

        return result;
    }

    jassertfalse;
    return JS_UNDEFINED;
}

//==============================================================================
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wsubobject-linkage")
struct JSFunctionArguments
{
    explicit JSFunctionArguments (qjs::JSContext* contextIn)
        : context (contextIn)
    {
    }

    JSFunctionArguments (qjs::JSContext* contextIn, const var::NativeFunctionArgs& args)
        : JSFunctionArguments (contextIn, Span { args.arguments, (size_t) args.numArguments })
    {
    }

    JSFunctionArguments (qjs::JSContext* contextIn, Span<const var> args)
        : context (contextIn)
    {
        values.reserve (args.size());

        for (const auto& arg : args)
            values.push_back (juceToQuickJs (arg, context));
    }

    ~JSFunctionArguments()
    {
        for (const auto& value : values)
            qjs::JS_FreeValue (context, value);
    }

    void add (const var& arg)
    {
        values.push_back (juceToQuickJs (arg, context));
    }

    qjs::JSValue* getArguments()
    {
        return values.data();
    }

    int getSize() const
    {
        return (int) values.size();
    }

private:
    qjs::JSContext* context;
    std::vector<qjs::JSValue> values;

    JUCE_DECLARE_NON_COPYABLE (JSFunctionArguments)
    JUCE_DECLARE_NON_MOVEABLE (JSFunctionArguments)
};
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

//==============================================================================
// Throws on failure
static var tryQuickJSToJuce (const qjs::QuickJSContext::ValuePtr& ptr,
                             const qjs::JSValue* parent = nullptr)
{
    using namespace qjs;

    jassert (ptr.context != nullptr);

    if (JS_IsUndefined (ptr.value))
        return var::undefined();

    if (JS_IsNull (ptr.value))
        return var{};

    if (JS_IsNumber (ptr.value))
    {
        double d = 0;
        JS_ToFloat64 (ptr.context, std::addressof (d), ptr.value);
        return d;
    }

    if (JS_IsBool (ptr.value))
        return JS_ToBool (ptr.context, ptr.value) != 0;

    if (JS_IsString (ptr.value))
    {
        size_t len = 0;
        const auto* s = JS_ToCStringLen2 (ptr.context, std::addressof (len), ptr.value, false);
        const ScopeGuard scope { [&] { JS_FreeCString (ptr.context, s); } };
        return String::fromUTF8 (s, (int) len);
    }

    if (JS_IsArray (ptr.context, ptr.value))
    {
        const auto lengthProp = ptr["length"];
        uint32_t len = 0;
        JS_ToUint32 (ptr.context, &len, lengthProp.get());

        Array<var> result;
        result.ensureStorageAllocated ((int) len);

        for (auto i = decltype (len){}; i < len; ++i)
            result.add (tryQuickJSToJuce (ptr[i], &ptr.value));

        return result;
    }

    if (JS_IsFunction (ptr.context, ptr.value))
    {
        // ValuePtr is move-only, so can't be captured into a std::function.
        // Use a custom copyable callable instead.
        struct Callable
        {
            Callable (JSContext* ctxIn, JSValue fnIn, JSValue selfIn)
                : ctx (ctxIn),
                  fn (JS_DupValue (ctx, fnIn)),
                  self (JS_DupValue (ctx, selfIn))
            {
            }

            Callable (const Callable& other)
                : ctx (other.ctx),
                  fn (JS_DupValue (ctx, other.fn)),
                  self (JS_DupValue (ctx, other.self))
            {
            }

            Callable& operator= (const Callable& other)
            {
                Callable { other }.swap (*this);
                return *this;
            }

            ~Callable()
            {
                JS_FreeValue (ctx, fn);
                JS_FreeValue (ctx, self);
            }

            void swap (Callable& other) noexcept
            {
                std::swap (other.ctx, ctx);
                std::swap (other.fn, fn);
                std::swap (other.self, self);
            }

            var operator() (const var::NativeFunctionArgs& args) const
            {
                JSFunctionArguments convertedArgs { ctx, args };

                const qjs::QuickJSContext::ValuePtr result { qjs::JS_Call (ctx,
                                                                           fn,
                                                                           self,
                                                                           (int) convertedArgs.getSize(),
                                                                           convertedArgs.getArguments()),
                                                             ctx };

                return discardError (quickJSToJuce (result));
            }

            JSContext* ctx{};
            JSValue fn, self;
        };

        const qjs::QuickJSContext::ValuePtr parentToUse { parent != nullptr ? JS_DupValue (ptr.context, *parent)
                                                                            : JS_GetGlobalObject (ptr.context),
                                                          ptr.context };

        return var::NativeFunction { Callable { ptr.context, ptr.value, parentToUse.value } };
    }

    if (JS_IsObject (ptr.value))
    {
        std::vector<std::string> propNames;

        for (auto obj = ptr.takeValue (JS_DupValue (ptr.context, ptr.value));;)
        {
            JSPropertyEnum* properties = nullptr;
            uint32_t numProps = 0;

            if (JS_GetOwnPropertyNames (ptr.context, &properties, &numProps, obj.get(), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0
                || properties == nullptr)
            {
                return new DynamicObject;
            }

            const ScopeGuard scope { [&] { js_free (ptr.context, properties); } };

            propNames.reserve (numProps);

            for (uint32_t i = 0; i < numProps; ++i)
            {
                const auto* name = JS_AtomToCString (ptr.context, properties[i].atom);
                std::string nameString (name);

                if (nameString != QuickJSContext::objectNameAttribute)
                    propNames.push_back (std::move (nameString));

                JS_FreeCString (ptr.context, name);
                JS_FreeAtom (ptr.context, properties[i].atom);
            }

            auto proto = ptr.takeValue (JS_GetPrototype (ptr.context, obj.get()));

            if (! JS_IsObject (proto.get()))
                break;

            obj = std::move (proto);
        }

        DynamicObject::Ptr result = new DynamicObject;

        for (auto& propName : propNames)
            result->setProperty (String (propName), tryQuickJSToJuce (ptr[propName.c_str()], &ptr.value));

        return result.get();
    }

    ptr.throwIfError();
    return {};
}

static VarOrError quickJSToJuce (const qjs::QuickJSContext::ValuePtr& ptr)
{
    try
    {
        return tryQuickJSToJuce (ptr);
    }
    catch (const detail::choc::javascript::Error& error)
    {
        return String (error.what());
    }
}

//==============================================================================
// Any type that references the QuickJS types inside the anonymous namespace added by us requires
// this with GCC. Suppressing this warning is fine, since these classes are only visible and used
// in a single translation unit.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wsubobject-linkage")
class detail::QuickJSWrapper
{
public:
    qjs::JSContext* getQuickJSContext() const
    {
        return impl->context;
    }

    qjs::JSRuntime* getQuickJSRuntime() const
    {
        return impl->runtime;
    }

    /*  Returning a value > 0 will interrupt the QuickJS engine.
    */
    void setInterruptHandler (std::function<int()> interruptHandlerIn)
    {
        interruptHandler = std::move (interruptHandlerIn);
        qjs::JS_SetInterruptHandler (getQuickJSRuntime(), handleInterrupt, (void*) this);
    }

private:
    static int handleInterrupt (qjs::JSRuntime*, void* opaque)
    {
        auto& self = *static_cast<QuickJSWrapper*> (opaque);

        if (self.interruptHandler != nullptr)
            return self.interruptHandler();

        return 0;
    }

    std::unique_ptr<qjs::QuickJSContext> impl = std::make_unique<qjs::QuickJSContext>();
    std::function<int()> interruptHandler;
};
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

using SetterFn = qjs::JSValue (*) (qjs::JSContext* ctx,
                                   qjs::JSValueConst thisVal,
                                   qjs::JSValueConst val,
                                   int magic);
using GetterFn = qjs::JSValue (*) (qjs::JSContext* ctx, qjs::JSValueConst thisVal, int magic);

// A replacement for the JS_CGETSET_MAGIC_DEF macro in QuickJS
static qjs::JSCFunctionListEntry makeFunctionListEntry (const char* name,
                                                        GetterFn getter,
                                                        SetterFn setter,
                                                        int16_t magic)
{
    qjs::JSCFunctionListEntry e { name, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, qjs::JS_DEF_CGETSET_MAGIC, magic, {} };
    e.u.getset.get.getter_magic = getter;
    e.u.getset.set.setter_magic = setter;
    return e;
}

// A replacement for the JS_UNDEFINED macro in QuickJS
static qjs::JSValue makeUndefined()
{
   #if defined(JS_NAN_BOXING) // Differentiates between 32 and 64 bit builds
    return (((uint64_t)(qjs::JS_TAG_UNDEFINED) << 32) | (uint32_t)(0));
   #else
    return qjs::JSValue (static_cast<int32_t> (0), qjs::JS_TAG_UNDEFINED);
   #endif
}

static qjs::JSClassID createClassId()
{
    // A passed in value of 0 asks QuickJS to allocate us a new unique ID. QuickJS uses global
    // variables for the bookkeeping, so it's safe to use this function to initialise globals.
    qjs::JSClassID newId = 0;
    return qjs::JS_NewClassID (&newId);
}

//==============================================================================
/*  Attached as an opaque pointer to the corresponding JS object. Its lifetime is managed by the
    QuickJS engine, which calls the finalise function when the corresponding JSValue is deleted.
*/
struct DynamicObjectWrapper
{
    DynamicObjectWrapper (detail::QuickJSWrapper& engineIn, DynamicObject::Ptr objectIn)
        : engine (engineIn), object (objectIn)
    {
        getDynamicObjects().insert (this);
    }

    int16_t getOrdinal (const Identifier& identifier)
    {
        if (const auto& it = ordinals.find (identifier); it != ordinals.end())
            return it->second;

        identifiers.emplace_back (identifier);
        const auto newSize = identifiers.size() - 1;
        jassert (newSize <= (size_t) std::numeric_limits<int16_t>::max());
        const auto newOrdinal = (int16_t) newSize;
        ordinals[identifier] = newOrdinal;
        return newOrdinal;
    }

    auto getIdentifier (int ordinal) const
    {
        jassert ((size_t) ordinal < identifiers.size());
        return identifiers[(size_t) ordinal];
    }

    NamedValueSet& getProperties() const
    {
        return object->getProperties();
    }

    static void finaliser (qjs::JSRuntime*, qjs::JSValue val)
    {
        auto* wrapper = static_cast<DynamicObjectWrapper*> (qjs::JS_GetOpaque (val, getClassId()));
        wrapper->finalise();
    }

    void finalise()
    {
        getDynamicObjects().erase (this);
        delete this;
    }

    static void createClass (qjs::JSRuntime* runtime)
    {
        qjs::JSClassDef classDef {};
        classDef.class_name = "juce_DynamicObject";
        classDef.finalizer  = finaliser;
        qjs::JS_NewClass (runtime, getClassId(), &classDef);
    }

    //==============================================================================
    static qjs::JSValue callDispatcher (qjs::JSContext* ctx,
                                        qjs::JSValueConst thisValue,
                                        int numArgs,
                                        qjs::JSValueConst* args,
                                        int ordinal)
    {
        auto& self = *static_cast<DynamicObjectWrapper*> (qjs::JS_GetOpaque2 (ctx, thisValue, getClassId()));
        const auto argList = quickJSToJuce (Span { args, (size_t) numArgs }, ctx);
        const auto identifier = self.getIdentifier (ordinal);
        auto result = self.object->invokeMethod (identifier,
                                                 { self.object.get(), argList.data(), (int) argList.size() });
        return juceToQuickJs (result, ctx);
    }

    static qjs::JSValue setDispatcher (qjs::JSContext* ctx,
                                       qjs::JSValueConst thisVal,
                                       qjs::JSValueConst val,
                                       int ordinal)
    {
        auto& self = *static_cast<DynamicObjectWrapper*> (qjs::JS_GetOpaque2 (ctx, thisVal, getClassId()));
        self.object->setProperty (self.getIdentifier (ordinal), discardError (quickJSToJuce ({ qjs::JS_DupValue (ctx, val), ctx })));

        // In case there is a problem we could return e.g. `JS_EXCEPTION` or
        // `JS_ThrowRangeError(ctx, "invalid precision");` here.
        return makeUndefined();
    }

    static qjs::JSValue getDispatcher (qjs::JSContext* ctx, qjs::JSValueConst thisVal, int ordinal)
    {
        auto& self = *static_cast<DynamicObjectWrapper*> (qjs::JS_GetOpaque2 (ctx, thisVal, getClassId()));
        return juceToQuickJs (self.object->getProperty (self.getIdentifier (ordinal)), ctx);
    }

    static qjs::JSClassID getClassId()
    {
        static qjs::JSClassID classId = createClassId();
        return classId;
    }

    static std::set<void*>& getDynamicObjects()
    {
        // Used to check if an opaque ptr attached to a JSValue is a DynamicObjectWrapper
        static std::set<void*> dynamicObjects;
        return dynamicObjects;
    }

    //==============================================================================
    detail::QuickJSWrapper& engine;
    DynamicObject::Ptr object;
    std::map<Identifier, int16_t> ordinals;
    std::vector<Identifier> identifiers;
};

//==============================================================================
class JavascriptEngine::Impl
{
public:
    using ValuePtr = qjs::QuickJSContext::ValuePtr;

    //==============================================================================
    Impl()
    {
        DynamicObjectWrapper::createClass (engine.getQuickJSRuntime());

        engine.setInterruptHandler ([this]
        {
            return (int64) Time::getMillisecondCounterHiRes() >= timeout;
        });
    }

    void registerNativeObject (const Identifier& name,
                               DynamicObject::Ptr dynamicObject,
                               std::optional<qjs::JSValue> parent = std::nullopt)
    {
        auto wrapper  = std::make_unique<DynamicObjectWrapper> (engine, dynamicObject);
        auto* ctx     = engine.getQuickJSContext();
        auto jsObject = JS_NewObjectClass (ctx, (int) DynamicObjectWrapper::getClassId());
        qjs::JS_SetOpaque (jsObject, (void*) wrapper.get());

        std::vector<qjs::JSCFunctionListEntry> propertyFunctionList;

        for (const auto& [identifier, prop] : wrapper->getProperties())
        {
            auto* jsIdentifier = identifier.toString().toRawUTF8();

            if (prop.isMethod())
            {
                qjs::JS_SetPropertyStr (ctx,
                                        jsObject,
                                        jsIdentifier,
                                        JS_NewCFunctionMagic (ctx,
                                                              DynamicObjectWrapper::callDispatcher,
                                                              jsIdentifier,
                                                              0,
                                                              qjs::JS_CFUNC_generic_magic,
                                                              wrapper->getOrdinal (identifier)));
            }
            else if (prop.isObject())
            {
                if (auto* embeddedObject = prop.getDynamicObject())
                    registerNativeObject (identifier, embeddedObject, jsObject);
            }
            else
            {
                const auto entry = makeFunctionListEntry (jsIdentifier,
                                                          DynamicObjectWrapper::getDispatcher,
                                                          DynamicObjectWrapper::setDispatcher,
                                                          wrapper->getOrdinal (identifier));
                propertyFunctionList.push_back (entry);
            }
        }

        if (! propertyFunctionList.empty())
        {
            qjs::JS_SetPropertyFunctionList (ctx,
                                             jsObject,
                                             propertyFunctionList.data(),
                                             (int) propertyFunctionList.size());
        }

        const auto jsObjectName = name.toString().toRawUTF8();

        if (parent.has_value())
        {
            qjs::JS_SetPropertyStr (ctx, *parent, jsObjectName, jsObject);
        }
        else
        {
            ValuePtr globalObject { qjs::JS_GetGlobalObject (ctx), ctx };
            qjs::JS_SetPropertyStr (ctx, globalObject.get(), jsObjectName, jsObject);
        }

        wrapper.release();
    }

    var evaluate (const String& code, Result* errorMessage, RelativeTime maxExecTime)
    {
        resetTimeout (maxExecTime);

        if (errorMessage != nullptr)
            *errorMessage = Result::ok();

        const auto result = quickJSToJuce ({ JS_Eval (engine.getQuickJSContext(), code.toRawUTF8(), code.getNumBytesAsUTF8(), "", JS_EVAL_TYPE_GLOBAL), engine.getQuickJSContext() });

        if (auto* v = std::get_if<var> (&result))
            return *v;

        if (auto* e = std::get_if<String> (&result))
            if (errorMessage != nullptr)
                *errorMessage = Result::fail (*e);

        return var::undefined();
    }

    Result execute (const String& code, RelativeTime maxExecTime)
    {
        auto result = Result::ok();
        evaluate (code, &result, maxExecTime);
        return result;
    }

    var callFunction (const Identifier& function,
                      const var::NativeFunctionArgs& args,
                      Result* errorMessage,
                      RelativeTime maxExecTime)
    {
        resetTimeout (maxExecTime);

        auto* ctx = engine.getQuickJSContext();
        const auto functionStr = function.toString();

        const auto fn = qjs::JS_NewAtomLen (ctx, functionStr.toRawUTF8(), functionStr.getNumBytesAsUTF8());

        JSFunctionArguments argList { ctx, args };

        qjs::QuickJSContext::ValuePtr global { JS_GetGlobalObject (ctx), ctx };
        qjs::QuickJSContext::ValuePtr returnVal { JS_Invoke (ctx, global.get(), fn, argList.getSize(), argList.getArguments()), ctx };

        JS_FreeAtom (ctx, fn);

        if (errorMessage != nullptr)
            *errorMessage = Result::ok();

        const auto result = quickJSToJuce (returnVal);

        if (auto* v = std::get_if<var> (&result))
            return *v;

        if (auto* e = std::get_if<String> (&result))
            if (errorMessage != nullptr)
                *errorMessage = Result::fail (*e);

        return var::undefined();
    }

    void stop() noexcept
    {
        timeout = (int64) Time::getMillisecondCounterHiRes();
    }

    JSObject getRootObject() const
    {
        return JSObject { &engine };
    }

private:
    //==============================================================================
    void resetTimeout (RelativeTime maxExecTime)
    {
        timeout = (int64) Time::getMillisecondCounterHiRes() + maxExecTime.inMilliseconds();
    }

    detail::QuickJSWrapper engine;
    std::atomic<int64> timeout{};
};

//==============================================================================
JavascriptEngine::JavascriptEngine()
    : maximumExecutionTime (15.0),
      impl (std::make_unique<Impl>())
{
}

JavascriptEngine::~JavascriptEngine() = default;

void JavascriptEngine::registerNativeObject (const Identifier& name, DynamicObject* object)
{
    impl->registerNativeObject (name, object);
}

Result JavascriptEngine::execute (const String& javascriptCode)
{
    return impl->execute (javascriptCode, maximumExecutionTime);
}

var JavascriptEngine::evaluate (const String& javascriptCode, Result* errorMessage)
{
    return impl->evaluate (javascriptCode, errorMessage, maximumExecutionTime);
}

var JavascriptEngine::callFunction (const Identifier& function,
                                    const var::NativeFunctionArgs& args,
                                    Result* errorMessage)
{
    return impl->callFunction (function, args, errorMessage, maximumExecutionTime);
}

void JavascriptEngine::stop() noexcept
{
    impl->stop();
}

JSObject JavascriptEngine::getRootObject() const
{
    return impl->getRootObject();
}

NamedValueSet JavascriptEngine::getRootObjectProperties() const
{
    return getRootObject().getProperties();
}

//==============================================================================
static bool hasProperty (qjs::JSContext* ctx, qjs::JSValueConst object, const char* name)
{
    qjs::JSAtom atom = JS_NewAtom (ctx, name);
    ScopeGuard freeAtom { [&] { qjs::JS_FreeAtom (ctx, atom); } };

    return JS_HasProperty (ctx, object, atom) > 0;
}

static qjs::JSValue getOrCreateProperty (qjs::JSContext* ctx, qjs::JSValueConst object, const char* name)
{
    if (! hasProperty (ctx, object, name))
        qjs::JS_SetPropertyStr (ctx, object, name, JS_NewObject (ctx));

    return qjs::JS_GetPropertyStr (ctx, object, name);
}

static uint32_t toUint32 (int64 value)
{
    jassert (0 <= value && value <= (int64) std::numeric_limits<uint32_t>::max());
    return (uint32_t) value;
}

//==============================================================================
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wsubobject-linkage")
class JSObject::Impl
{
public:
    using ValuePtr = qjs::QuickJSContext::ValuePtr;

    explicit Impl (const detail::QuickJSWrapper* engineIn)
        : Impl (engineIn,
                { qjs::JS_GetGlobalObject (engineIn->getQuickJSContext()), engineIn->getQuickJSContext() })
    {
    }

    Impl (const Impl& other)
        : Impl (other.engine,
                { qjs::JS_DupValue (other.engine->getQuickJSContext(), other.valuePtr.get()),
                  other.engine->getQuickJSContext() })
    {
    }

    std::unique_ptr<Impl> getChild (const Identifier& prop) const
    {
        return rawToUniquePtr (new Impl (engine, { getOrCreateProperty (engine->getQuickJSContext(),
                                                                        valuePtr.get(),
                                                                        prop.toString().toRawUTF8()),
                                                   engine->getQuickJSContext() }));
    }

    std::unique_ptr<Impl> getChild (int64 index) const
    {
        jassert (isArray());
        return rawToUniquePtr (new Impl (engine, valuePtr[toUint32 (index)]));
    }

    bool hasProperty (const Identifier& name) const
    {
        return juce::hasProperty (engine->getQuickJSContext(), valuePtr.get(), name.toString().toRawUTF8());
    }

    void setProperty (const Identifier& name, const var& value) const
    {
        auto* ctx = engine->getQuickJSContext();

        qjs::JS_SetPropertyStr (ctx, valuePtr.get(), name.toString().toRawUTF8(), juceToQuickJs (value, ctx));
    }

    void setProperty (int64 index, const var& value) const
    {
        auto* ctx = engine->getQuickJSContext();

        qjs::JS_SetPropertyInt64 (ctx, valuePtr.get(), index, juceToQuickJs (value, ctx));
    }

    var get() const
    {
        if (auto* opaque = qjs::JS_GetOpaque (valuePtr.get(), DynamicObjectWrapper::getClassId()))
            if (DynamicObjectWrapper::getDynamicObjects().count (opaque) != 0)
                return { static_cast<DynamicObjectWrapper*> (opaque)->object.get() };

        auto* ctx = engine->getQuickJSContext();
        return discardError (quickJSToJuce ({ qjs::JS_DupValue (ctx, valuePtr.get()), ctx }));
    }

    VarOrError invokeMethod (const Identifier& methodName, Span<const var> args) const
    {
        if (! hasProperty (methodName))
        {
            jassertfalse;
            return {};
        }

        auto* ctx = engine->getQuickJSContext();
        const auto methodAtom = JS_NewAtom (ctx, methodName.toString().toRawUTF8());
        ScopeGuard scope { [&] { qjs::JS_FreeAtom (ctx, methodAtom); } };

        JSFunctionArguments arguments { ctx, args };

        ValuePtr returnVal { qjs::JS_Invoke (ctx,
                                             valuePtr.get(),
                                             methodAtom,
                                             arguments.getSize(),
                                             arguments.getArguments()),
                             ctx };

        return quickJSToJuce (returnVal);
    }

    NamedValueSet getProperties() const
    {
        NamedValueSet result;

        auto* ctx = engine->getQuickJSContext();
        ValuePtr names { qjs::JS_GetOwnPropertyNames2 (ctx,
                                                       valuePtr.get(),
                                                       qjs::JS_GPN_ENUM_ONLY | qjs::JS_GPN_STRING_MASK,
                                                       qjs::JS_ITERATOR_KIND_KEY),
                         ctx };


        if (auto v = discardError (quickJSToJuce (names)); const auto* propertyNames = v.getArray())
        {
            for (const auto& name : *propertyNames)
            {
                if (name.isString())
                {
                    const Identifier prop { name.toString() };
                    result.set (prop, getChild (prop)->get());
                }
            }
        }

        return result;
    }

    bool isArray() const
    {
        return qjs::JS_IsArray (engine->getQuickJSContext(), valuePtr.get());
    }

    int64 getSize() const
    {
        if (! isArray())
        {
            jassertfalse;
            return 0;
        }

        auto lengthProp = valuePtr["length"];
        uint32_t length = 0;
        qjs::JS_ToUint32 (engine->getQuickJSContext(), &length, lengthProp.get());
        return (int64) length;
    }

private:
    Impl (const detail::QuickJSWrapper* e, ValuePtr&& ptr)
        : engine (e), valuePtr (std::move (ptr))
    {
    }

    const detail::QuickJSWrapper* engine = nullptr;
    ValuePtr valuePtr;
};
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

JSObject::JSObject (const detail::QuickJSWrapper* engine)
    : impl (new Impl (engine))
{
}

JSObject::JSObject (std::unique_ptr<Impl> implIn)
    : impl (std::move (implIn))
{
}

JSObject::JSObject (const JSObject& other)
    : impl (new Impl (*other.impl))
{
}

JSObject::~JSObject() = default;

JSObject::JSObject (JSObject&&) noexcept = default;

JSObject& JSObject::operator= (const JSObject& other)
{
    JSObject { other }.swap (*this);
    return *this;
}

JSObject& JSObject::operator= (JSObject&& other) noexcept = default;

JSObject JSObject::getChild (const Identifier& name) const
{
    return JSObject { impl->getChild (name) };
}

JSObject JSObject::operator[] (const Identifier& name) const
{
    return getChild (name);
}

bool JSObject::isArray() const
{
    return impl->isArray();
}

int64 JSObject::getSize() const
{
    return impl->getSize();
}

JSObject JSObject::getChild (int64 index) const
{
    jassert (isArray());
    return JSObject { impl->getChild (index) };
}

JSObject JSObject::operator[] (int64 index) const
{
    return getChild (index);
}

bool JSObject::hasProperty (const Identifier& name) const
{
    return impl->hasProperty (name);
}

var JSObject::get() const
{
    return impl->get();
}

void JSObject::setProperty (const Identifier& name, const var& value) const
{
    impl->setProperty (name, value);
}

void JSObject::setProperty (int64 index, const var& value) const
{
    impl->setProperty (index, value);
}

var JSObject::invokeMethod (const Identifier& methodName,
                            Span<const var> args,
                            Result* result) const
{
    const auto varOrError = impl->invokeMethod (methodName, args);

    if (result != nullptr)
    {
        const auto* e = std::get_if<String> (&varOrError);
        *result = e != nullptr ? Result::fail (*e) : Result::ok();
    }

    return discardError (varOrError);
}

NamedValueSet JSObject::getProperties() const
{
    return impl->getProperties();
}

void JSObject::swap (JSObject& other) noexcept
{
    std::swap (impl, other.impl);
}

//==============================================================================
JSCursor::JSCursor (JSObject rootIn) : root (std::move (rootIn))
{
}

var JSCursor::get() const
{
    if (const auto resolved = getFullResolution())
        return resolved->get();

    return var::undefined();
}

void JSCursor::set (const var& value) const
{
    const auto resolved = getPartialResolution();

    if (! resolved.has_value())
    {
        jassertfalse;  // Can't resolve an Object to change along the path stored in the cursor
        return;
    }

    const auto& [object, property] = *resolved;

    if (! property.has_value())
    {
        jassertfalse;  // Can't set the value of the root Object
        return;
    }

    if (auto* prop = std::get_if<Identifier> (&(*property)))
    {
        object.setProperty (*prop, value);
        return;
    }

    if (auto* prop = std::get_if<int64> (&(*property)))
    {
        object.setProperty (*prop, value);
        return;
    }
}

JSCursor JSCursor::getChild (const Identifier& name) const
{
    auto copy = *this;
    copy.path.emplace_back (name);
    return copy;
}

JSCursor JSCursor::operator[] (const Identifier& name) const
{
    return getChild (name);
}

JSCursor JSCursor::getChild (int64 index) const
{
    auto copy = *this;
    copy.path.emplace_back (index);
    return copy;
}

JSCursor JSCursor::operator[] (int64 index) const
{
    return getChild (index);
}

JSObject JSCursor::getOrCreateObject() const
{
    const auto resolved = getPartialResolution();
    jassert (resolved.has_value());

    const auto& [object, property] = *resolved;

    if (! property.has_value())
        return object;

    auto* integerValue = std::get_if<int64> (&(*property));

    jassert   (integerValue == nullptr
            || (object.isArray() && (*integerValue) < object.getSize()));

    if (integerValue != nullptr)
        return object[*integerValue];

    auto* prop = std::get_if<Identifier> (&(*property));
    jassert(prop != nullptr);
    return object[*prop];
}

bool JSCursor::isValid() const
{
    return getPartialResolution().has_value();
}

bool JSCursor::isArray() const
{
    if (auto resolved = getFullResolution())
        return resolved->isArray();

    return false;
}

var JSCursor::invoke (Span<const var> args, Result* result) const
{
    const auto resolved = getPartialResolution();

    if (! resolved.has_value())
    {
        jassertfalse;
        return {};
    }

    const auto& [object, property] = *resolved;
    if (! property.has_value())
    {
        jassertfalse;
        return {};
    }

    return object.invokeMethod (*std::get_if<Identifier> (&(*property)), args, result);
}

std::optional<JSObject> JSCursor::resolve (JSObject object, Property property)
{
    if (auto* index = std::get_if<int64> (&property))
    {
        if (! object.isArray())
            return std::nullopt;

        if (! (*index < object.getSize()))
            return std::nullopt;

        return object[*index];
    }

    if (auto* key = std::get_if<Identifier> (&property))
    {
        if (! object.hasProperty (*key))
            return std::nullopt;

        return object[*key];
    }

    jassertfalse;
    return std::nullopt;
}

std::optional<JSCursor::PartialResolution> JSCursor::getPartialResolution() const
{
    auto object = root;

    for (int i = 0, iEnd = (int) path.size() - 1; i < iEnd; ++i)
    {
        const auto& property = path[(size_t) i];
        auto objectOpt = resolve (object, property);

        if (! objectOpt.has_value())
            return std::nullopt;

        object = *objectOpt;
    }

    return std::make_optional<PartialResolution> (std::move (object),
                                                  path.empty() ? std::nullopt
                                                               : std::make_optional (path.back()));
}

std::optional<JSObject> JSCursor::getFullResolution() const
{
    if (auto partiallyResolved = getPartialResolution())
    {
        if (! partiallyResolved->second.has_value())
            return partiallyResolved->first;

        return resolve (partiallyResolved->first, *(partiallyResolved->second));
    }

    return std::nullopt;
}

} // namespace juce