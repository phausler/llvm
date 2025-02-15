//===- SpecialCaseListTest.cpp - Unit tests for SpecialCaseList -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SpecialCaseList.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class SpecialCaseListTest : public ::testing::Test {
protected:
  SpecialCaseList *makeSpecialCaseList(StringRef List, std::string &Error) {
    std::unique_ptr<MemoryBuffer> MB = MemoryBuffer::getMemBuffer(List);
    return SpecialCaseList::create(MB.get(), Error);
  }

  SpecialCaseList *makeSpecialCaseList(StringRef List) {
    std::string Error;
    SpecialCaseList *SCL = makeSpecialCaseList(List, Error);
    assert(SCL);
    assert(Error == "");
    return SCL;
  }
};

TEST_F(SpecialCaseListTest, Basic) {
  std::unique_ptr<SpecialCaseList> SCL(
      makeSpecialCaseList("# This is a comment.\n"
                          "\n"
                          "src:hello\n"
                          "src:bye\n"
                          "src:hi=category\n"
                          "src:z*=category\n"));
  EXPECT_TRUE(SCL->inSection("src", "hello"));
  EXPECT_TRUE(SCL->inSection("src", "bye"));
  EXPECT_TRUE(SCL->inSection("src", "hi", "category"));
  EXPECT_TRUE(SCL->inSection("src", "zzzz", "category"));
  EXPECT_FALSE(SCL->inSection("src", "hi"));
  EXPECT_FALSE(SCL->inSection("fun", "hello"));
  EXPECT_FALSE(SCL->inSection("src", "hello", "category"));
}

TEST_F(SpecialCaseListTest, GlobalInitCompat) {
  std::unique_ptr<SpecialCaseList> SCL(
      makeSpecialCaseList("global:foo=init\n"));
  EXPECT_FALSE(SCL->inSection("global", "foo"));
  EXPECT_FALSE(SCL->inSection("global", "bar"));
  EXPECT_TRUE(SCL->inSection("global", "foo", "init"));
  EXPECT_FALSE(SCL->inSection("global", "bar", "init"));

  SCL.reset(makeSpecialCaseList("global-init:foo\n"));
  EXPECT_FALSE(SCL->inSection("global", "foo"));
  EXPECT_FALSE(SCL->inSection("global", "bar"));
  EXPECT_TRUE(SCL->inSection("global", "foo", "init"));
  EXPECT_FALSE(SCL->inSection("global", "bar", "init"));

  SCL.reset(makeSpecialCaseList("type:t2=init\n"));
  EXPECT_FALSE(SCL->inSection("type", "t1"));
  EXPECT_FALSE(SCL->inSection("type", "t2"));
  EXPECT_FALSE(SCL->inSection("type", "t1", "init"));
  EXPECT_TRUE(SCL->inSection("type", "t2", "init"));

  SCL.reset(makeSpecialCaseList("global-init-type:t2\n"));
  EXPECT_FALSE(SCL->inSection("type", "t1"));
  EXPECT_FALSE(SCL->inSection("type", "t2"));
  EXPECT_FALSE(SCL->inSection("type", "t1", "init"));
  EXPECT_TRUE(SCL->inSection("type", "t2", "init"));

  SCL.reset(makeSpecialCaseList("src:hello=init\n"));
  EXPECT_FALSE(SCL->inSection("src", "hello"));
  EXPECT_FALSE(SCL->inSection("src", "bye"));
  EXPECT_TRUE(SCL->inSection("src", "hello", "init"));
  EXPECT_FALSE(SCL->inSection("src", "bye", "init"));

  SCL.reset(makeSpecialCaseList("global-init-src:hello\n"));
  EXPECT_FALSE(SCL->inSection("src", "hello"));
  EXPECT_FALSE(SCL->inSection("src", "bye"));
  EXPECT_TRUE(SCL->inSection("src", "hello", "init"));
  EXPECT_FALSE(SCL->inSection("src", "bye", "init"));
}

TEST_F(SpecialCaseListTest, Substring) {
  std::unique_ptr<SpecialCaseList> SCL(makeSpecialCaseList("src:hello\n"
                                                           "fun:foo\n"
                                                           "global:bar\n"));
  EXPECT_FALSE(SCL->inSection("src", "othello"));
  EXPECT_FALSE(SCL->inSection("fun", "tomfoolery"));
  EXPECT_FALSE(SCL->inSection("global", "bartender"));

  SCL.reset(makeSpecialCaseList("fun:*foo*\n"));
  EXPECT_TRUE(SCL->inSection("fun", "tomfoolery"));
  EXPECT_TRUE(SCL->inSection("fun", "foobar"));
}

TEST_F(SpecialCaseListTest, InvalidSpecialCaseList) {
  std::string Error;
  EXPECT_EQ(nullptr, makeSpecialCaseList("badline", Error));
  EXPECT_EQ("Malformed line 1: 'badline'", Error);
  EXPECT_EQ(nullptr, makeSpecialCaseList("src:bad[a-", Error));
  EXPECT_EQ("Malformed regex in line 1: 'bad[a-': invalid character range",
            Error);
  EXPECT_EQ(nullptr, makeSpecialCaseList("src:a.c\n"
                                   "fun:fun(a\n",
                                   Error));
  EXPECT_EQ("Malformed regex in line 2: 'fun(a': parentheses not balanced",
            Error);
  EXPECT_EQ(nullptr, SpecialCaseList::create("unexisting", Error));
  EXPECT_EQ(0U, Error.find("Can't open file 'unexisting':"));
}

TEST_F(SpecialCaseListTest, EmptySpecialCaseList) {
  std::unique_ptr<SpecialCaseList> SCL(makeSpecialCaseList(""));
  EXPECT_FALSE(SCL->inSection("foo", "bar"));
}

}


