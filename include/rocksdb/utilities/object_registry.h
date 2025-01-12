// Copyright (c) 2016-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#ifndef ROCKSDB_LITE

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/status.h"
#include "rocksdb/utilities/regex.h"

namespace ROCKSDB_NAMESPACE {
class Logger;
class ObjectLibrary;

// Returns a new T when called with a string. Populates the std::unique_ptr
// argument if granting ownership to caller.
template <typename T>
using FactoryFunc =
    std::function<T*(const std::string&, std::unique_ptr<T>*, std::string*)>;

// The signature of the function for loading factories
// into an object library.  This method is expected to register
// factory functions in the supplied ObjectLibrary.
// The ObjectLibrary is the library in which the factories will be loaded.
// The std::string is the argument passed to the loader function.
// The RegistrarFunc should return the number of objects loaded into this
// library
using RegistrarFunc = std::function<int(ObjectLibrary&, const std::string&)>;

class ObjectLibrary {
 public:
  // Base class for an Entry in the Registry.
  class Entry {
   public:
    virtual ~Entry() {}
    Entry(const std::string& name) : name_(std::move(name)) {}

    // Checks to see if the target matches this entry
    virtual bool matches(const std::string& target) const {
      return name_ == target;
    }
    const std::string& Name() const { return name_; }

   private:
    const std::string name_;  // The name of the Entry
  };                          // End class Entry

  // An Entry containing a FactoryFunc for creating new Objects
  //
  // !!!!!! WARNING !!!!!!: The implementation currently uses std::regex, which
  // has terrible performance in some cases, including possible crash due to
  // stack overflow. See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61582
  // for example. Avoid complicated regexes as much as possible.
  template <typename T>
  class FactoryEntry : public Entry {
   public:
    FactoryEntry(const std::string& name, FactoryFunc<T> f)
        : Entry(name), factory_(std::move(f)) {
      // FIXME: the API needs to expose this failure mode. For now, bad regexes
      // will match nothing.
      Regex::Parse(name, &regex_).PermitUncheckedError();
    }
    ~FactoryEntry() override {}
    bool matches(const std::string& target) const override {
      return regex_.Matches(target);
    }
    // Creates a new T object.
    T* NewFactoryObject(const std::string& target, std::unique_ptr<T>* guard,
                        std::string* msg) const {
      return factory_(target, guard, msg);
    }

   private:
    Regex regex_;  // The pattern for this entry
    FactoryFunc<T> factory_;
  };  // End class FactoryEntry
 public:
  explicit ObjectLibrary(const std::string& id) { id_ = id; }

  const std::string& GetID() const { return id_; }
  // Finds the entry matching the input name and type
  const Entry* FindEntry(const std::string& type,
                         const std::string& name) const;

  // Returns the total number of factories registered for this library.
  // This method returns the sum of all factories registered for all types.
  // @param num_types returns how many unique types are registered.
  size_t GetFactoryCount(size_t* num_types) const;

  void Dump(Logger* logger) const;

  // Registers the factory with the library for the pattern.
  // If the pattern matches, the factory may be used to create a new object.
  template <typename T>
  const FactoryFunc<T>& Register(const std::string& pattern,
                                 const FactoryFunc<T>& factory) {
    std::unique_ptr<Entry> entry(new FactoryEntry<T>(pattern, factory));
    AddEntry(T::Type(), entry);
    return factory;
  }

  // Invokes the registrar function with the supplied arg for this library.
  int Register(const RegistrarFunc& registrar, const std::string& arg) {
    return registrar(*this, arg);
  }

  // Returns the default ObjectLibrary
  static std::shared_ptr<ObjectLibrary>& Default();

 private:
  // Adds the input entry to the list for the given type
  void AddEntry(const std::string& type, std::unique_ptr<Entry>& entry);

  // Protects the entry map
  mutable std::mutex mu_;
  // ** FactoryFunctions for this loader, organized by type
  std::unordered_map<std::string, std::vector<std::unique_ptr<Entry>>> entries_;

  // The name for this library
  std::string id_;
};

// The ObjectRegistry is used to register objects that can be created by a
// name/pattern at run-time where the specific implementation of the object may
// not be known in advance.
class ObjectRegistry {
 public:
  static std::shared_ptr<ObjectRegistry> NewInstance();
  static std::shared_ptr<ObjectRegistry> NewInstance(
      const std::shared_ptr<ObjectRegistry>& parent);
  static std::shared_ptr<ObjectRegistry> Default();
  explicit ObjectRegistry(const std::shared_ptr<ObjectRegistry>& parent)
      : parent_(parent) {}

  std::shared_ptr<ObjectLibrary> AddLibrary(const std::string& id) {
    auto library = std::make_shared<ObjectLibrary>(id);
    libraries_.push_back(library);
    return library;
  }

  void AddLibrary(const std::shared_ptr<ObjectLibrary>& library) {
    libraries_.push_back(library);
  }

  void AddLibrary(const std::string& id, const RegistrarFunc& registrar,
                  const std::string& arg) {
    auto library = AddLibrary(id);
    library->Register(registrar, arg);
  }

  // Creates a new T using the factory function that was registered with a
  // pattern that matches the provided "target" string according to
  // std::regex_match.
  //
  // WARNING: some regexes are problematic for std::regex; see
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61582 for example
  //
  // If no registered functions match, returns nullptr. If multiple functions
  // match, the factory function used is unspecified.
  //
  // Populates res_guard with result pointer if caller is granted ownership.
  template <typename T>
  T* NewObject(const std::string& target, std::unique_ptr<T>* guard,
               std::string* errmsg) {
    guard->reset();
    const auto* basic = FindEntry(T::Type(), target);
    if (basic != nullptr) {
      const auto* factory =
          static_cast<const ObjectLibrary::FactoryEntry<T>*>(basic);
      return factory->NewFactoryObject(target, guard, errmsg);
    } else {
      *errmsg = std::string("Could not load ") + T::Type();
      return nullptr;
    }
  }

  // Creates a new unique T using the input factory functions.
  // Returns OK if a new unique T was successfully created
  // Returns NotSupported if the type/target could not be created
  // Returns InvalidArgument if the factory return an unguarded object
  //                      (meaning it cannot be managed by a unique ptr)
  template <typename T>
  Status NewUniqueObject(const std::string& target,
                         std::unique_ptr<T>* result) {
    std::string errmsg;
    T* ptr = NewObject(target, result, &errmsg);
    if (ptr == nullptr) {
      return Status::NotSupported(errmsg, target);
    } else if (*result) {
      return Status::OK();
    } else {
      return Status::InvalidArgument(std::string("Cannot make a unique ") +
                                         T::Type() + " from unguarded one ",
                                     target);
    }
  }

  // Creates a new shared T using the input factory functions.
  // Returns OK if a new shared T was successfully created
  // Returns NotSupported if the type/target could not be created
  // Returns InvalidArgument if the factory return an unguarded object
  //                      (meaning it cannot be managed by a shared ptr)
  template <typename T>
  Status NewSharedObject(const std::string& target,
                         std::shared_ptr<T>* result) {
    std::string errmsg;
    std::unique_ptr<T> guard;
    T* ptr = NewObject(target, &guard, &errmsg);
    if (ptr == nullptr) {
      return Status::NotSupported(errmsg, target);
    } else if (guard) {
      result->reset(guard.release());
      return Status::OK();
    } else {
      return Status::InvalidArgument(std::string("Cannot make a shared ") +
                                         T::Type() + " from unguarded one ",
                                     target);
    }
  }

  // Creates a new static T using the input factory functions.
  // Returns OK if a new static T was successfully created
  // Returns NotSupported if the type/target could not be created
  // Returns InvalidArgument if the factory return a guarded object
  //                      (meaning it is managed by a unique ptr)
  template <typename T>
  Status NewStaticObject(const std::string& target, T** result) {
    std::string errmsg;
    std::unique_ptr<T> guard;
    T* ptr = NewObject(target, &guard, &errmsg);
    if (ptr == nullptr) {
      return Status::NotSupported(errmsg, target);
    } else if (guard.get()) {
      return Status::InvalidArgument(std::string("Cannot make a static ") +
                                         T::Type() + " from a guarded one ",
                                     target);
    } else {
      *result = ptr;
      return Status::OK();
    }
  }

  // Dump the contents of the registry to the logger
  void Dump(Logger* logger) const;

 private:
  explicit ObjectRegistry(const std::shared_ptr<ObjectLibrary>& library) {
    libraries_.push_back(library);
  }

  const ObjectLibrary::Entry* FindEntry(const std::string& type,
                                        const std::string& name) const;

  // The set of libraries to search for factories for this registry.
  // The libraries are searched in reverse order (back to front) when
  // searching for entries.
  std::vector<std::shared_ptr<ObjectLibrary>> libraries_;
  std::shared_ptr<ObjectRegistry> parent_;
};
}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
