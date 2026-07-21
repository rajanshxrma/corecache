// Explicit template instantiation, compiled into exactly one translation
// unit. corecache is header-only (an INTERFACE library has nothing to
// compile on its own), so this file exists purely to force the compiler to
// fully instantiate the template bodies at least once, surfacing any
// warnings/errors that would otherwise stay latent until a consumer
// happens to instantiate the same combination.
#include "corecache/cache.hpp"

#include <string>

template class corecache::Cache<int, int, corecache::LruPolicy<int>>;
template class corecache::Cache<int, int, corecache::ArcPolicy<int>>;
template class corecache::Cache<std::string, std::string, corecache::LruPolicy<std::string>>;
template class corecache::Cache<std::string, std::string, corecache::ArcPolicy<std::string>>;
