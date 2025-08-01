#pragma once

#include "visitor.h"

#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/stream/output.h>

namespace NKikimr::NChangeExchange {

class IChangeRecord: public TThrRefBase {
public:
    using TPtr = TIntrusivePtr<IChangeRecord>;

    enum class ESource: ui8 {
        Unspecified = 0,
        InitialScan = 1,
    };

    enum class EKind: ui8 {
        AsyncIndex = 0,
        CdcDataChange = 1,
        CdcHeartbeat = 2 ,
        IncrementalRestore = 3,
        CdcSchemaChange = 4,
    };

public:
    virtual ui64 GetOrder() const = 0;
    virtual ui64 GetGroup() const = 0;
    virtual ui64 GetStep() const = 0;
    virtual ui64 GetTxId() const = 0;
    virtual EKind GetKind() const = 0;
    virtual const TString& GetBody() const = 0;
    virtual ESource GetSource() const = 0;
    virtual const TString& GetSourceId() const = 0;
    virtual bool IsBroadcast() const = 0;

    virtual void Accept(IVisitor& visitor) const = 0;

    virtual void RewriteTxId(ui64 value) = 0;

    virtual TString ToString() const = 0;
    virtual void Out(IOutputStream& out) const = 0;

}; // IChangeRecord

template <typename T, typename TDerived>
class TChangeRecordBuilder;

class TChangeRecordBase: public IChangeRecord {
    template <typename T, typename TDerived>
    friend class TChangeRecordBuilder;

public:
    ui64 GetOrder() const override { return Order; }
    const TString& GetBody() const override { return Body; }
    ESource GetSource() const override { return Source; }
    const TString& GetSourceId() const override { return SourceId; }
    bool IsBroadcast() const override { return false; }

    void RewriteTxId(ui64) override { Y_ABORT("not implemented"); }

    TString ToString() const override;
    void Out(IOutputStream& out) const override;

protected:
    ui64 Order = Max<ui64>();
    TString Body;
    ESource Source = ESource::Unspecified;
    TString SourceId;

}; // TChangeRecordBase

template <typename T, typename TDerived>
class TChangeRecordBuilder {
protected:
    using TBase = TChangeRecordBuilder<T, TDerived>;
    using TSelf = TDerived;
    using EKind = IChangeRecord::EKind;
    using ESource = IChangeRecord::ESource;

    T* GetRecord() {
        return static_cast<T*>(Record.Get());
    }

public:
    TChangeRecordBuilder()
        : Record(MakeIntrusive<T>())
    {}

    explicit TChangeRecordBuilder(TIntrusivePtr<T> record)
        : Record(std::move(record))
    {}

    TSelf& WithOrder(ui64 order) {
        GetRecord()->Order = order;
        return static_cast<TSelf&>(*this);
    }

    TSelf& WithBody(const TString& body) {
        GetRecord()->Body = body;
        return static_cast<TSelf&>(*this);
    }

    TSelf& WithBody(TString&& body) {
        GetRecord()->Body = std::move(body);
        return static_cast<TSelf&>(*this);
    }

    TSelf& WithSource(ESource source) {
        GetRecord()->Source = source;
        return static_cast<TSelf&>(*this);
    }

    TIntrusivePtr<T> Build() {
        return Record;
    }

protected:
    TIntrusivePtr<T> Record;

}; // TChangeRecordBuilder

}

Y_DECLARE_OUT_SPEC(inline, NKikimr::NChangeExchange::IChangeRecord::TPtr, out, value) {
    return value->Out(out);
}

Y_DECLARE_OUT_SPEC(inline, NKikimr::NChangeExchange::IChangeRecord*, out, value) {
    return value->Out(out);
}
