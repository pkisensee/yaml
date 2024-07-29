///////////////////////////////////////////////////////////////////////////////
//
//  yaml.h
//
//  Copyright © Pete Isensee (PKIsensee@msn.com).
//  All rights reserved worldwide.
//
//  Permission to copy, modify, reproduce or redistribute this source code is
//  granted provided the above copyright notice is retained in the resulting 
//  source code.
// 
//  This software is provided "as is" and without any express or implied
//  warranties.
//
///////////////////////////////////////////////////////////////////////////////

#pragma once
#include <array>
#include <cassert>
#include <string>
#include <stack>

#include "Util.h"

namespace PKIsensee
{

struct YamlHandler
{
  virtual ~YamlHandler() {}
  virtual void onStartDocument() {}
  virtual void onEndDocument() {}
  virtual void onStartSequence() {}
  virtual void onEndSequence() {}
  virtual void onStartMapping() {}
  virtual void onEndMapping() {}
  virtual bool onKey( std::string_view ) { return true; } // true to continue; false to stop
  virtual bool onScalar( std::string_view ) { return true; } // true to continue; false to stop
  virtual void onError( std::string_view, [[maybe_unused]] size_t line, 
                                          [[maybe_unused]] size_t col ) {}
};

class YamlParser
{
public:

  YamlParser() = delete;
  YamlParser( const YamlParser& ) = delete;
  YamlParser( YamlParser&& ) = delete;
  YamlParser& operator=( const YamlParser& ) = delete;
  YamlParser&& operator=( YamlParser&& ) = delete;

  YamlParser( std::string_view, YamlHandler& );
  bool Parse();

private:

  struct Indent
  {
    size_t level = 0;
    bool isSequence = false;
  };

  // Helper to manage simple YAML indent stack; mimics std::stack API
  class YamlStack
  {
    static constexpr size_t kMaxStackSize = 32u;
  public:
    void push( Indent indent )
    {
      assert( mSize < kMaxStackSize - 1 );
      mStack[ mSize++ ] = indent;
    }
    void pop()
    {
      assert( mSize != 0 );
      --mSize;
    }
    Indent top() const
    {
      assert( mSize != 0 );
      return mStack[ mSize - 1 ];
    }
    bool empty() const
    {
      return mSize == 0;
    }
    size_t size() const
    {
      return mSize;
    }  
  private:
    std::array<Indent, kMaxStackSize> mStack;
    size_t mSize = 0u;
  };

  bool Error( std::string_view ) const;
  void Push( Indent );
  bool Pop();
  char PeekNext() const;
  Indent GetIndent();
  bool SkipStartDocument();
  void SkipSpaces();
  void SkipLine();
  void HandleMissingNull();
  bool IsNormalChar() const;
  bool ParseNode();
  bool ParsePlain();
  bool ParseQuoted( char );
  bool OutputScalar( std::string_view );

private:

  const char*  curr_;        // current YAML char being evaluated
  const char*  end_;         // one beyond last char of YAML text
  size_t       line_ = 1u;   // YAML line number
  size_t       col_ = 0u;    // YAML column number
  YamlHandler& yamlHandler_; // callbacks
  YamlStack    yamlStack_;   // current indentation level
  bool         completeKeyValuePair_ = true;

}; // class YamlParser

///////////////////////////////////////////////////////////////////////////////

namespace Yaml {

struct Special
{
  size_t firstSpecialPos = 0;  // only valid if hasSpecialChars; -1 otherwise
  size_t firstSingleQuote = 0; // only valid if hasSpecialChars; -1 if no single quotes
  size_t firstDoubleQuote = 0; // only valid if hasSpecialChars; -1 if no double quotes
  bool hasSpecialChars = false;
  char specialChar = '\0';

  explicit Special( bool hasSpecial ) : hasSpecialChars( hasSpecial ) {}
  Special() = default;
};

Special GetSpecialChars( std::string_view );
std::string CreateSafeScalar( std::string_view );
std::string CreateKeyValue( std::string_view tag, std::string_view scalar );

// Given an input container, creates a YAML formatted output sequence
// e.g. "['first','second','third']"

template <typename Container>
std::string CreateSequence( const Container& c )
requires Util::IsContainer<Container>
{
  if( c.size() == 0 )
    return "[]";

  std::string yaml{ "[" };
  if( c.size() == 1 )
  {
    if constexpr( Util::IsNumeric<typename Container::value_type> )
      yaml += Util::ToString( c.front() );
    else
      yaml += CreateSafeScalar( c.front() );
    yaml += ']';
    return yaml;
  }

  bool isFirstEntry = true;
  for( const auto& s : c )
  {
    if( !isFirstEntry )
      yaml += ", ";
    if constexpr( Util::IsNumeric<typename Container::value_type> )
      yaml += Util::ToString(s);
    else
      yaml += CreateSafeScalar(s);
    isFirstEntry = false;
  }
  yaml += ']';
  return yaml;
}

template <typename Container>
std::string CreateKeyValueSeq( std::string_view tag, const Container& c )
requires Util::IsContainer<Container>
{
  std::string yaml;
  yaml += tag;
  yaml += ": ";
  yaml += CreateSequence( c );
  yaml += '\n';
  return yaml;
}

} // end namespace Yaml

} // end namespace PKIsensee

///////////////////////////////////////////////////////////////////////////////
