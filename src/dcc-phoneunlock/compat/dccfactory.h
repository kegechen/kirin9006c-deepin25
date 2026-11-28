/*
 * 没装 dde-control-center-dev 时用的 DccFactory 声明。
 * 必须和 dde-control-center（deepin 25，6.1.x）/usr/include/dde-control-center/dccfactory.h 二进制一致：
 * 类名、命名空间、虚函数顺序、IID 都不能改——控制中心按 IID 认插件，再 qobject_cast 成 DccFactory 调虚函数。
 * DccFactory 本身的元对象和虚表在 libdde-control-center.so 里，插件要链它（-ldde-control-center），
 * 这个头文件也不能交给 moc（否则会多一份 DccFactory 的元对象）。
 * Makefile 里 -I/usr/include/dde-control-center 排在前面，装了官方头文件就用官方的。
 */
#ifndef DCCFACTORY_H
#define DCCFACTORY_H

#include <QObject>

namespace dccV25 {
#define DccFactory_iid "org.deepin.dde.dcc-factory/v1.0"
class DccObject;

class DccFactory : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    /* 返回的对象在 xxxMain.qml 里是 dccData */
    virtual QObject *create(QObject * = nullptr) { return nullptr; }

    /* 不用 QML 时自己造 DccObject 返回（这里不用） */
    virtual DccObject *dccObject(QObject * = nullptr) { return nullptr; }
};
} // namespace dccV25
Q_DECLARE_INTERFACE(dccV25::DccFactory, DccFactory_iid)

#endif // DCCFACTORY_H
