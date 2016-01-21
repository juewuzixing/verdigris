#include <QtCore/QObject>
#include <tuple>
#include <utility>


// This would go in the qobjectdefs.h header with the other defines
template <typename Type, typename... T> struct QProperty : std::tuple<T...> {
    using std::tuple<T...>::tuple;
    using PropertyType = Type;
};
template <typename Type, typename... T> constexpr auto qt_makeProperty(T&& ...t)
{ return QProperty<Type, typename std::decay<T>::type...>{ std::forward<T>(t)... }; }

#define Q_PROPERTY2(TYPE, NAME, ...) static constexpr auto qt_property_##NAME = \
qt_makeProperty<TYPE>(__VA_ARGS__);


/*-----------------------------------------------------------------------------------------------*/
/* Helpers to play with tuple or strings at compile time                                         */
/*-----------------------------------------------------------------------------------------------*/


/**
 * define index_sequence and make_index_sequence  since I don't have C++14 headers
 */
template<int...I> struct index_sequence {
    /** Added an operator+  to concatenate two sequences */
    template<int... J> constexpr index_sequence<I...,J...> operator+(index_sequence<J...>) const { return {}; }
};
template<int I> struct make_integer_sequence_helper {
    template<int... Is> static index_sequence<Is..., I-1> func(index_sequence<Is...>);
    using result = decltype(func(typename make_integer_sequence_helper<I-1>::result()));
};
template<> struct make_integer_sequence_helper<0> { using result = index_sequence<>; };
template<int I> using make_index_sequence = typename make_integer_sequence_helper<I>::result;


/**
 *  tuple_tail()  Returns a tuple with the first element removed
 */
template<typename T, int...I> constexpr auto tuple_tail_helper(const T&t , index_sequence<I...>) {
    return std::make_tuple(std::get<I+1>(t)...);
}
template<typename T> constexpr auto tuple_tail(const T& tuple) {
    return tuple_tail_helper(tuple, make_index_sequence<std::tuple_size<T>::value-1>());
}


/** zip():
 * make tuple<pair<A1, B1>, pair<A2, B2>, ...> from two tuples tuple<A1, A2, ...> and tuple<B1, B2, ...>
 */
template<typename A, typename B, int... I>
constexpr auto zip_helper(A a, B b, index_sequence<I...>) {
    return std::make_tuple( std::make_pair(std::get<I>(a), std::get<I>(b))... );
}
template<typename A, typename B>
constexpr auto zip(A a, B b) {
    static_assert(std::tuple_size<A>::value == std::tuple_size<B>::value,
                  "arguments must be tuples of the same sizes");
    return zip_helper(a, b, make_index_sequence<std::tuple_size<A>::value>());
}



/*
 * Helpers to play with static strings
 */

/** A compile time character array of size N  */
template<int N> using StaticStringArray = const char [N];

/** Represents a string of size N  (N includes the 0 at the end) */
template<int N> struct StaticString  {
    StaticStringArray<N> data;
    template <int... I>
    constexpr StaticString(StaticStringArray<N> &d, index_sequence<I...>) : data{ (d[I])... } { }
    constexpr StaticString(StaticStringArray<N> &d) : StaticString(d, make_index_sequence<N>()) {}
    static constexpr int size = N;
    constexpr char operator[](int p) const { return data[p]; }
};

/* A tuple containing many  StaticString with possibly different sizes */
template<int ...Sizes> using StaticStringList = std::tuple<StaticString<Sizes>...>;

/* Creates a StaticStringList from a list of string literal */
template<int... N>
constexpr StaticStringList<N...> makeStaticStringList(StaticStringArray<N> & ...args)  {
    return std::make_tuple(StaticString<N>(args)...);
}

/** concatenate() : returns a StaticString which is the concatenation of all the strings in a StaticStringList
 *     Note:  keeps the \0 between the strings
 */
template<typename I1, typename I2> struct concatenate_helper;
template<int... I1, int... I2> struct concatenate_helper<index_sequence<I1...>, index_sequence<I2...>> {
    static constexpr int size = sizeof...(I1) + sizeof...(I2);
    static constexpr auto concatenate(const StaticString<sizeof...(I1)> &s1, const StaticString<sizeof...(I2)> &s2) {
        StaticStringArray<size> d = { s1[I1]... , s2[I2]... };
        return StaticString<size>( d );
    }
};
constexpr StaticString<1> concatenate(const StaticStringList<>) { return ""; }
template<int H,  int... T> constexpr auto concatenate(const StaticStringList<H, T...> &s) {
    auto tail = concatenate(tuple_tail(s));
    return concatenate_helper<make_index_sequence<H>, make_index_sequence<tail.size>>::concatenate(std::get<0>(s), tail);
}

/** Add a string in a StaticStringList */
template<int L, int...N >
constexpr auto addString(const StaticStringList<N...> &l, const StaticString<L> & s) {
    return std::tuple_cat(l, std::make_tuple(s));
}


/*-----------*/

template<int N>
struct cs_number : public cs_number<N - 1>
{
    static constexpr int value = N;
    static constexpr cs_number<N-1> prev() { return {}; }
};

template<>
struct cs_number<0>
{ static constexpr int value = 0; };



/*-----------------------------------------------------------------------------------------------*/
/* The code that generates the QMetaObject                                                          */
/*-----------------------------------------------------------------------------------------------*/
namespace MetaObjectBuilder {
    /** Holds information about a method */
    template<typename F, int NameLength>
    struct MetaMethodInfo {
        enum { Signal, Slot } type;
        enum { Public, Protected, Private } access;
        F func;
        StaticString<NameLength> name;
        static constexpr int argCount = QtPrivate::FunctionPointer<F>::ArgumentCount;
        using ReturnType = typename QtPrivate::FunctionPointer<F>::ReturnType;

        template<typename T>
        void metacall(T *_o, QMetaObject::Call _c, void **_a) const {
            if (_c == QMetaObject::InvokeMetaMethod) {
                using P = QtPrivate::FunctionPointer<F>;
                P::template call<typename P::Arguments, ReturnType>(func, _o, _a);
            }
        }
    };

    template<typename F, int N>
    constexpr MetaMethodInfo<F, N> makeMetaSlotInfo(F f, StaticStringArray<N> &name)
    { return { MetaMethodInfo<F, N>::Slot, MetaMethodInfo<F, N>::Public /* FIXME */, f, name }; }

    /** Holds information about a property */
    template<typename Type, int NameLength, typename Getter, typename Setter, typename Member, typename Notify>
    struct MetaPropertyInfo {
        using PropertyType = Type;
        StaticString<NameLength> name;
        Getter getter;
        Setter setter;
        Member member;
        Notify notify;
        uint flags = 0;

        template <typename S> constexpr auto setGetter(const S&s) const
        { return MetaPropertyInfo<Type, NameLength, S, Getter, Member, Notify>{name, s, setter, member, notify, flags}; }
        template <typename S> constexpr auto setSetter(const S&s) const
        { return MetaPropertyInfo<Type, NameLength, Getter, S, Member, Notify>{name, getter, s, member, notify, flags}; }
        template <typename S> constexpr auto setMember(const S&s) const
        { return MetaPropertyInfo<Type, NameLength, Getter, Setter, S, Notify>{name, getter, setter, s, notify, flags}; }

        template<typename T>
        void metacall(T *_o, QMetaObject::Call _c, void **_a) const {
            switch(+_c) {
            case QMetaObject::ReadProperty:
                if (getter) {
                    *reinterpret_cast<Type*>(_a[0]) = (_o->*getter)();
                } else if (member) {
                    *reinterpret_cast<Type*>(_a[0]) = _o->*member;
                }
                break;
            case QMetaObject::WriteProperty:
                if (setter) {
                    (_o->*setter)(*reinterpret_cast<Type*>(_a[0]));
                } else if (member) {
                    _o->*member = *reinterpret_cast<Type*>(_a[0]);
                }
            }
        }
    };

#if 0
    /** Just a dummy pointer to member function to be used when there is none */
    template<typename T> struct DummyFunctPointer { operator bool() const { return false; } };
    template<typename T, typename O> T &operator->*(O*, DummyFunctPointer<T>) { return *static_cast<T*>(nullptr);  }

    /** Parse a property and fill a MetaPropertyInfo */
    template <typename PropInfo> constexpr auto parseProperty(const PropInfo &p, const std::tuple<> &) { return p; }
    template <typename PropInfo, typename Obj, typename Arg, typename Ret, typename... Tail>
    constexpr auto parseProperty(const PropInfo &p, const std::tuple<Ret (Obj::*)(Arg), Tail...> &t) {
        return parseProperty(p.setSetter(std::get<0>(t)) ,tuple_tail(t));
    }
    template <typename PropInfo, typename Obj, typename Ret, typename... Tail>
    constexpr auto parseProperty(const PropInfo &p, const std::tuple<Ret (Obj::*)(), Tail...> &t) {
        return parseProperty(p.setGetter(std::get<0>(t)) ,tuple_tail(t));
    }
    template <typename PropInfo, typename Obj, typename Ret, typename... Tail>
    constexpr auto parseProperty(const PropInfo &p, const std::tuple<Ret (Obj::*), Tail...> &t) {
        return parseProperty(p.setMember(std::get<0>(t)) ,tuple_tail(t));
    }

    template<typename P, int... I>
    constexpr auto makePropertyInfo(const StaticString<sizeof...(I)+12> &n,  const P &p, index_sequence<I...>) {
        constexpr int nSize = sizeof...(I);
        StaticStringArray<nSize> d = { n[I+12]... };
        using Type = typename P::PropertyType;
        using Dummy = DummyFunctPointer<Type>;
        MetaPropertyInfo<Type, sizeof...(I), Dummy, Dummy, Dummy, Dummy> meta { d };
        return parseProperty(meta, p);
    }
#endif

    /** Holds information about a class,  includeing all the properties and methods */
    template<int NameLength, typename Methods, typename Properties>
    struct ClassInfo {
        StaticString<NameLength> name;
        Methods methods;
        Properties properties;

        template<typename P>
        constexpr auto addProperty(const P&p) const {
            auto newP = std::tuple_cat(properties, std::make_tuple(p));
            return ClassInfo<NameLength, Methods, decltype(newP)>{ name, methods, newP };
        }

        static constexpr int methodCount = std::tuple_size<Methods>::value;
        static constexpr int propertyCount = std::tuple_size<Properties>::value;
    };
    /** Construct a ClassInfo with just the name */
    template<typename T, int N>
    constexpr auto makeClassInfo(StaticStringArray<N> &name)
        -> ClassInfo<N, decltype(T::w_MethodState(cs_number<255>{})), std::tuple<>>
    { return { name, T::w_MethodState(cs_number<255>{}), {} }; }


    /**
     * generate...
     *  Create the metaobject's integer data array
     *  (as a index_sequence)
     * returns std::pair<StaticStringList, index_sequence>:  the modified strings and the array of strings
     */
    template<int, typename Strings>
    constexpr auto generateMethods(const Strings &s, const std::tuple<>&) {
        return std::make_pair(s, index_sequence<>());
    }
    template<int ParamIndex, typename Strings, typename Method, typename... Tail>
    constexpr auto generateMethods(const Strings &s, const std::tuple<Method, Tail...> &t) {

        auto method = std::get<0>(t);
        auto s2 = addString(s, method.name);

        using thisMethod = index_sequence<std::tuple_size<Strings>::value, //name
                                          Method::argCount,
                                          ParamIndex, //parametters
                                          1, //tag, always \0
                                          0x0a /* hardcoded flags: Public */
                                        >;

        auto next = generateMethods<ParamIndex + 1 + Method::argCount * 2>(s2, tuple_tail(t));
        return std::make_pair(next.first, thisMethod() + next.second);
    }
#if 0
    template<typename Strings>
    constexpr auto generateProperties(const Strings &s, const std::tuple<>&) {
        return std::make_pair(s, index_sequence<>());
    }
    template<typename Strings, typename Prop, typename... Tail>
    constexpr auto generateProperties(const Strings &s, const std::tuple<Prop, Tail...> &t) {

        auto prop = std::get<0>(t);
        auto s2 = addString(s, prop.name);

        using thisProp = index_sequence<std::tuple_size<Strings>::value, //name
                                        qMetaTypeId<typename Prop::PropertyType>(),
                                        0x03 /* hardcoded flags: Public */
                                        >;

        auto next = generateProperties(s2, tuple_tail(t));
        return std::make_pair(next.first, thisProp() + next.second);

    }


    //Helper class for generateSingleMethodParameter:  generate the parametter array
    template<typename ...Args> struct HandleArgsHelper { using Result = index_sequence<>; };
    template<typename A, typename... Args>
    struct HandleArgsHelper<A, Args...> {
        using Result =  decltype(index_sequence<qMetaTypeId<A>(), 1>() + typename HandleArgsHelper<Args...>::Result());
    };

    template<typename Strings, typename Obj, typename Ret, typename... Args>
    constexpr auto generateSingleMethodParameter(const Strings &ss, Ret (Obj::*)(Args...) ) {
        constexpr int retTyp = qMetaTypeId<Ret>();
        return std::make_pair(ss, index_sequence<retTyp>() + typename HandleArgsHelper<Args...>::Result());
    }

    template<typename Strings>
    constexpr auto generateMethodsParameters(const Strings &s, const std::tuple<>&) {
        return std::make_pair(s, index_sequence<>());
    }
    template<typename Strings, typename Method, typename... Tail>
    constexpr auto generateMethodsParameters(const Strings &s, const std::tuple<Method, Tail...> &t) {
        auto method = std::get<0>(t);
        auto thisMethod = generateSingleMethodParameter(s, method.func);
        auto next = generateMethodsParameters(thisMethod.first, tuple_tail(t));
        return std::make_pair(next.first, thisMethod.second + next.second);
    }

#endif
    // generate the integer array and the lists of string
    template<typename CI>
    constexpr auto generateDataArray(const CI &classInfo) {
        constexpr int methodOffset = 14;
        constexpr int propertyOffset = methodOffset + CI::methodCount * 5;
        constexpr int paramIndex = propertyOffset + CI::propertyCount * 3 ;
        using header = index_sequence<
                7,       // revision
                0,       // classname
                0,    0, // classinfo
                CI::methodCount,   methodOffset, // methods
                CI::propertyCount,    propertyOffset, // properties
                0,    0, // enums/sets
                0,    0, // constructors
                0,       // flags
                CI::methodCount   // signalCount  /* Yes, everything is considered signal for now */
        >;
        auto stringData = std::make_tuple(classInfo.name, StaticString<1>(""));
        auto methods = generateMethods<paramIndex>(stringData , classInfo.methods);
     //   auto properties = generateProperties(methods.first , classInfo.properties);
     //   auto parametters = generateMethodsParameters(properties.first, classInfo.methods);
        return std::make_pair(methods.first,  header()  + methods.second /*+ properties.second + parametters.second*/);
    }


    /**
     * Entry class for the generation of the QMetaObject
     */
/*
    template<typename T> struct MetaObjectCreatorHelper {
    private:
//         static constexpr auto names = typename_<T>::staticStringList();
//         static constexpr auto values = typedef_<T>::tuple();
//         static constexpr auto zipped = zip(values, names);

    public:

//        using Base =  typename std::remove_reference<decltype(*std::get<1>(values))>::type;

        static constexpr auto classInfo = makeClassInfo<T>();
        static constexpr auto data = generateDataArray(classInfo);
        static constexpr auto string_data = data.first;
        static constexpr auto int_data = data.second;
    };*/


    /**
     * Holder for the string data.  Just like in the moc generated code.
     */
    template<int N, int L> struct qt_meta_stringdata_t {
         QByteArrayData data[N];
         char stringdata[L];
    };

    /** Builds the string data
     * \param S: a index_sequence that goes from 0 to the fill size of the strings
     * \param I: a index_sequence that goes from 0 to the number of string
     * \param O: a index_sequence of the offsets
     * \param N: a index_sequence of the size of each strings
     * \param T: the MetaObjectCreatorHelper
     */
    template<typename S, typename I, typename O, typename N, typename T> struct BuildStringDataHelper;
    template<int... S, int... I, int... O, int...N, typename T>
    struct BuildStringDataHelper<index_sequence<S...>, index_sequence<I...>, index_sequence<O...>, index_sequence<N...>, T> {
        using meta_stringdata_t = const qt_meta_stringdata_t<sizeof...(I), sizeof...(S)>;
        static meta_stringdata_t qt_meta_stringdata;
    };
    template<int... S, int... I, int... O, int...N, typename T>
    const qt_meta_stringdata_t<sizeof...(I), sizeof...(S)>
    BuildStringDataHelper<index_sequence<S...>, index_sequence<I...>, index_sequence<O...>, index_sequence<N...>, T>::qt_meta_stringdata = {
        {Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(N-1,
                qptrdiff(offsetof(meta_stringdata_t, stringdata) + O - I * sizeof(QByteArrayData)) )...},
        { concatenate(T::string_data)[S]...     }
    };



    /* Compute the sum of many integers */
    constexpr int sums() { return 0; }
    template<typename... Args>
    constexpr int sums(int i, Args... args) { return i + sums(args...);  }

    /**
     * Given N a list of string sizes, compute the list offsets to each of the strings.
     */
    template<int... N> struct ComputeOffsets;
    template<> struct ComputeOffsets<> {
        using Result = index_sequence<>;
    };
    template<int H, int... T> struct ComputeOffsets<H, T...> {
        template<int ... I> static index_sequence<0, (I+H)...> func(index_sequence<I...>);
        using Result = decltype(func(typename ComputeOffsets<T...>::Result()));
    };

    /**
     * returns the string data suitable for the QMetaObject from a list of string
     * T is MetaObjectCreatorHelper<ObjectType>
     */
    template<typename T, int... N>
    constexpr const QByteArrayData *build_string_data(StaticStringList<N...>)  {
        return BuildStringDataHelper<make_index_sequence<sums(N...)>,
                                      make_index_sequence<sizeof...(N)>,
                                      typename ComputeOffsets<N...>::Result,
                                      index_sequence<N...>,
                                      T>
            ::qt_meta_stringdata.data;
    }

    /**
     * returns a pointer to an array of string built at compile time.
     */
    template<typename I> struct build_int_data;
    template<int... I> struct build_int_data<index_sequence<I...>> {
        static const uint data[sizeof...(I)];
    };
    template<int... I> const uint build_int_data<index_sequence<I...>>::data[sizeof...(I)] = { I... };

#if 0
    /**
     * calls metacall on each element of the tuple
     */
    template<typename T> void metacall(T *, QMetaObject::Call, int , void** , const std::tuple<> &) {}
    template<typename T, typename Ms> void metacall(T *_o, QMetaObject::Call _c, int _id, void** _a, const Ms &ms) {
        if (_id == 0) {
            std::get<0>(ms).metacall(_o, _c, _a);
        } else {
           metacall(_o, _c, _id-1, _a, tuple_tail(ms));
        }
    }

    /**
     * Helper for QMetaObject::IndexOfMethod
     */
    void indexOfMethod (int *, void **, int, const std::tuple<> &) {}
    template<typename Ms> void indexOfMethod (int *result, void **func, int _id, const Ms &ms) {
        auto f = std::get<0>(ms).func;
        if (f == *reinterpret_cast<decltype(f)*>(func)) {
            *result = _id;
        } else {
            indexOfMethod(result, func, _id+1, tuple_tail(ms));
        }
    }
#endif
}

template<typename T>
constexpr QMetaObject createMetaObject()
{

    using Creator = typename T::MetaObjectCreatorHelper;

    auto string_data = MetaObjectBuilder::build_string_data<Creator>(Creator::string_data);
    auto int_data = MetaObjectBuilder::build_int_data<typename std::remove_const<decltype(Creator::int_data)>::type>::data;

    return { { &T::W_BaseType::staticMetaObject , string_data , int_data,  T::qt_static_metacall }  };
}


template<typename T> int qt_metacall_impl(T *_o, QMetaObject::Call _c, int _id, void** _a) {
#if 0
    using Creator = MetaObjectBuilder::MetaObjectCreatorHelper<T>;
    _id = _o->Creator::Base::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod || _c == QMetaObject::RegisterMethodArgumentMetaType) {
        constexpr int methodCount = Creator::classInfo.methodCount;
        if (_id < methodCount)
            T::qt_static_metacall(_o, _c, _id, _a);
        _id -= methodCount;
    } else if ((_c >= QMetaObject::ReadProperty && _c <= QMetaObject::QueryPropertyUser)
                || _c == QMetaObject::RegisterPropertyMetaType) {
        constexpr auto ps = Creator::classInfo.properties;
        MetaObjectBuilder::metacall(_o, _c, _id, _a, ps);
    }
#endif
    return _id;
}

template<typename T> void qt_static_metacall_impl(QObject *_o, QMetaObject::Call _c, int _id, void** _a) {
#if 0
    constexpr auto ms = MetaObjectBuilder::MetaObjectCreatorHelper<T>::classInfo.methods;
    if (_c == QMetaObject::InvokeMetaMethod || _c == QMetaObject::RegisterMethodArgumentMetaType) {
        Q_ASSERT(T::staticMetaObject.cast(_o));
        MetaObjectBuilder::metacall(static_cast<T*>(_o), _c, _id, _a, ms);
    } else if (_c == QMetaObject::IndexOfMethod) {
        MetaObjectBuilder::indexOfMethod(reinterpret_cast<int *>(_a[0]), reinterpret_cast<void **>(_a[1]), 0, ms);
    }
#endif
}


template<typename T> T getParentObjectHelper(void* (T::*)(const char*));


#define W_OBJECT(TYPE) \
        using W_ThisType = TYPE; /* This is the only reason why we need TYPE */ \
        template<typename T> friend constexpr QMetaObject createMetaObject(); \
        static constexpr std::tuple<> w_MethodState(cs_number<0>) { return {}; } \
    public: \
        struct MetaObjectCreatorHelper; \
        using W_BaseType = decltype(getParentObjectHelper(&W_ThisType::qt_metacast)); \
    Q_OBJECT


#define W_SLOT_1(access, ...) \
    __VA_ARGS__; \

//     static constexpr const int CS_TOKENPASTE2(cs_counter_value, __LINE__) =  \
//     decltype(cs_counter(cs_number<255>{}))::value; \
//     static constexpr cs_number<CS_TOKENPASTE2(cs_counter_value, __LINE__) + 1>  \
//     cs_counter(cs_number<CS_TOKENPASTE2(cs_counter_value, __LINE__) + 1>) \
//     {  \
//     return cs_number<CS_TOKENPASTE2(cs_counter_value, __LINE__) + 1>{};      \
//     }  \
//     static void cs_regTrigger(cs_number<CS_TOKENPASTE2(cs_counter_value, __LINE__)>) \
//     {  \
//     const char *va_args = #__VA_ARGS__;    \
//     QMetaMethod::Access accessType = QMetaMethod::access; \
//     constexpr int cntValue = CS_TOKENPASTE2(cs_counter_value, __LINE__);
//     // do not remove the ";", this is required for part two of the macro


#define W_SLOT_2(slotName) \
    static constexpr auto w_MethodState(cs_number<std::tuple_size<decltype(w_MethodState(cs_number<255>{}))>::value+1> counter) \
    -> decltype(std::tuple_cat(w_MethodState(counter.prev()), std::make_tuple(MetaObjectBuilder::makeMetaSlotInfo(&W_ThisType::slotName, #slotName)))) \
        { return std::tuple_cat(w_MethodState(counter.prev()), std::make_tuple(MetaObjectBuilder::makeMetaSlotInfo(&W_ThisType::slotName, #slotName))); }




#define W_OBJECT_IMPL(TYPE) \
    struct TYPE::MetaObjectCreatorHelper { \
        static constexpr auto classInfo = MetaObjectBuilder::makeClassInfo<TYPE>(#TYPE); \
        static constexpr auto data = generateDataArray(classInfo); \
        static constexpr auto string_data = data.first; \
        static constexpr auto int_data = data.second; \
    }; \
    constexpr const QMetaObject TYPE::staticMetaObject = createMetaObject<TYPE>(); \
    const QMetaObject *TYPE::metaObject() const  { return &staticMetaObject; } \
    void *TYPE::qt_metacast(const char *) { return nullptr; } /* TODO */ \
    int TYPE::qt_metacall(QMetaObject::Call _c, int _id, void** _a) { \
        return qt_metacall_impl<TYPE>(this, _c, _id, _a); \
    } \
    void TYPE::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void** _a) { \
        qt_static_metacall_impl<TYPE>(_o, _c, _id, _a); \
    } \

