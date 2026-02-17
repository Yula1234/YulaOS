#ifndef LIB_CPP_TYPE_TRAITS_H
#define LIB_CPP_TYPE_TRAITS_H

namespace kernel {

template<typename T>
struct remove_reference {
    using type = T;
};

template<typename T>
struct remove_reference<T&> {
    using type = T;
};

template<typename T>
struct remove_reference<T&&> {
    using type = T;
};

template<typename T>
using remove_reference_t = typename remove_reference<T>::type;

}

#endif
