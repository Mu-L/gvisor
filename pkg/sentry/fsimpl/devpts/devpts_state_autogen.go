// automatically generated by stateify.

package devpts

import (
	"gvisor.dev/gvisor/pkg/state"
)

func (x *FilesystemType) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.FilesystemType"
}

func (x *FilesystemType) StateFields() []string {
	return []string{}
}

func (x *FilesystemType) beforeSave() {}

func (x *FilesystemType) StateSave(m state.Sink) {
	x.beforeSave()
}

func (x *FilesystemType) afterLoad() {}

func (x *FilesystemType) StateLoad(m state.Source) {
}

func (x *filesystem) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.filesystem"
}

func (x *filesystem) StateFields() []string {
	return []string{
		"Filesystem",
		"devMinor",
	}
}

func (x *filesystem) beforeSave() {}

func (x *filesystem) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.Filesystem)
	m.Save(1, &x.devMinor)
}

func (x *filesystem) afterLoad() {}

func (x *filesystem) StateLoad(m state.Source) {
	m.Load(0, &x.Filesystem)
	m.Load(1, &x.devMinor)
}

func (x *rootInode) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.rootInode"
}

func (x *rootInode) StateFields() []string {
	return []string{
		"implStatFS",
		"AlwaysValid",
		"InodeAttrs",
		"InodeDirectoryNoNewChildren",
		"InodeNotSymlink",
		"OrderedChildren",
		"rootInodeRefs",
		"locks",
		"dentry",
		"master",
		"root",
		"replicas",
		"nextIdx",
	}
}

func (x *rootInode) beforeSave() {}

func (x *rootInode) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.implStatFS)
	m.Save(1, &x.AlwaysValid)
	m.Save(2, &x.InodeAttrs)
	m.Save(3, &x.InodeDirectoryNoNewChildren)
	m.Save(4, &x.InodeNotSymlink)
	m.Save(5, &x.OrderedChildren)
	m.Save(6, &x.rootInodeRefs)
	m.Save(7, &x.locks)
	m.Save(8, &x.dentry)
	m.Save(9, &x.master)
	m.Save(10, &x.root)
	m.Save(11, &x.replicas)
	m.Save(12, &x.nextIdx)
}

func (x *rootInode) afterLoad() {}

func (x *rootInode) StateLoad(m state.Source) {
	m.Load(0, &x.implStatFS)
	m.Load(1, &x.AlwaysValid)
	m.Load(2, &x.InodeAttrs)
	m.Load(3, &x.InodeDirectoryNoNewChildren)
	m.Load(4, &x.InodeNotSymlink)
	m.Load(5, &x.OrderedChildren)
	m.Load(6, &x.rootInodeRefs)
	m.Load(7, &x.locks)
	m.Load(8, &x.dentry)
	m.Load(9, &x.master)
	m.Load(10, &x.root)
	m.Load(11, &x.replicas)
	m.Load(12, &x.nextIdx)
}

func (x *implStatFS) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.implStatFS"
}

func (x *implStatFS) StateFields() []string {
	return []string{}
}

func (x *implStatFS) beforeSave() {}

func (x *implStatFS) StateSave(m state.Sink) {
	x.beforeSave()
}

func (x *implStatFS) afterLoad() {}

func (x *implStatFS) StateLoad(m state.Source) {
}

func (x *lineDiscipline) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.lineDiscipline"
}

func (x *lineDiscipline) StateFields() []string {
	return []string{
		"size",
		"inQueue",
		"outQueue",
		"termios",
		"column",
	}
}

func (x *lineDiscipline) beforeSave() {}

func (x *lineDiscipline) StateSave(m state.Sink) {
	x.beforeSave()
	if !state.IsZeroValue(&x.masterWaiter) {
		state.Failf("masterWaiter is %#v, expected zero", &x.masterWaiter)
	}
	if !state.IsZeroValue(&x.replicaWaiter) {
		state.Failf("replicaWaiter is %#v, expected zero", &x.replicaWaiter)
	}
	m.Save(0, &x.size)
	m.Save(1, &x.inQueue)
	m.Save(2, &x.outQueue)
	m.Save(3, &x.termios)
	m.Save(4, &x.column)
}

func (x *lineDiscipline) afterLoad() {}

func (x *lineDiscipline) StateLoad(m state.Source) {
	m.Load(0, &x.size)
	m.Load(1, &x.inQueue)
	m.Load(2, &x.outQueue)
	m.Load(3, &x.termios)
	m.Load(4, &x.column)
}

func (x *outputQueueTransformer) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.outputQueueTransformer"
}

func (x *outputQueueTransformer) StateFields() []string {
	return []string{}
}

func (x *outputQueueTransformer) beforeSave() {}

func (x *outputQueueTransformer) StateSave(m state.Sink) {
	x.beforeSave()
}

func (x *outputQueueTransformer) afterLoad() {}

func (x *outputQueueTransformer) StateLoad(m state.Source) {
}

func (x *inputQueueTransformer) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.inputQueueTransformer"
}

func (x *inputQueueTransformer) StateFields() []string {
	return []string{}
}

func (x *inputQueueTransformer) beforeSave() {}

func (x *inputQueueTransformer) StateSave(m state.Sink) {
	x.beforeSave()
}

func (x *inputQueueTransformer) afterLoad() {}

func (x *inputQueueTransformer) StateLoad(m state.Source) {
}

func (x *masterInode) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.masterInode"
}

func (x *masterInode) StateFields() []string {
	return []string{
		"implStatFS",
		"InodeAttrs",
		"InodeNoopRefCount",
		"InodeNotDirectory",
		"InodeNotSymlink",
		"locks",
		"dentry",
		"root",
	}
}

func (x *masterInode) beforeSave() {}

func (x *masterInode) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.implStatFS)
	m.Save(1, &x.InodeAttrs)
	m.Save(2, &x.InodeNoopRefCount)
	m.Save(3, &x.InodeNotDirectory)
	m.Save(4, &x.InodeNotSymlink)
	m.Save(5, &x.locks)
	m.Save(6, &x.dentry)
	m.Save(7, &x.root)
}

func (x *masterInode) afterLoad() {}

func (x *masterInode) StateLoad(m state.Source) {
	m.Load(0, &x.implStatFS)
	m.Load(1, &x.InodeAttrs)
	m.Load(2, &x.InodeNoopRefCount)
	m.Load(3, &x.InodeNotDirectory)
	m.Load(4, &x.InodeNotSymlink)
	m.Load(5, &x.locks)
	m.Load(6, &x.dentry)
	m.Load(7, &x.root)
}

func (x *masterFileDescription) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.masterFileDescription"
}

func (x *masterFileDescription) StateFields() []string {
	return []string{
		"vfsfd",
		"FileDescriptionDefaultImpl",
		"LockFD",
		"inode",
		"t",
	}
}

func (x *masterFileDescription) beforeSave() {}

func (x *masterFileDescription) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.vfsfd)
	m.Save(1, &x.FileDescriptionDefaultImpl)
	m.Save(2, &x.LockFD)
	m.Save(3, &x.inode)
	m.Save(4, &x.t)
}

func (x *masterFileDescription) afterLoad() {}

func (x *masterFileDescription) StateLoad(m state.Source) {
	m.Load(0, &x.vfsfd)
	m.Load(1, &x.FileDescriptionDefaultImpl)
	m.Load(2, &x.LockFD)
	m.Load(3, &x.inode)
	m.Load(4, &x.t)
}

func (x *queue) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.queue"
}

func (x *queue) StateFields() []string {
	return []string{
		"readBuf",
		"waitBuf",
		"waitBufLen",
		"readable",
		"transformer",
	}
}

func (x *queue) beforeSave() {}

func (x *queue) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.readBuf)
	m.Save(1, &x.waitBuf)
	m.Save(2, &x.waitBufLen)
	m.Save(3, &x.readable)
	m.Save(4, &x.transformer)
}

func (x *queue) afterLoad() {}

func (x *queue) StateLoad(m state.Source) {
	m.Load(0, &x.readBuf)
	m.Load(1, &x.waitBuf)
	m.Load(2, &x.waitBufLen)
	m.Load(3, &x.readable)
	m.Load(4, &x.transformer)
}

func (x *replicaInode) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.replicaInode"
}

func (x *replicaInode) StateFields() []string {
	return []string{
		"implStatFS",
		"InodeAttrs",
		"InodeNoopRefCount",
		"InodeNotDirectory",
		"InodeNotSymlink",
		"locks",
		"dentry",
		"root",
		"t",
	}
}

func (x *replicaInode) beforeSave() {}

func (x *replicaInode) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.implStatFS)
	m.Save(1, &x.InodeAttrs)
	m.Save(2, &x.InodeNoopRefCount)
	m.Save(3, &x.InodeNotDirectory)
	m.Save(4, &x.InodeNotSymlink)
	m.Save(5, &x.locks)
	m.Save(6, &x.dentry)
	m.Save(7, &x.root)
	m.Save(8, &x.t)
}

func (x *replicaInode) afterLoad() {}

func (x *replicaInode) StateLoad(m state.Source) {
	m.Load(0, &x.implStatFS)
	m.Load(1, &x.InodeAttrs)
	m.Load(2, &x.InodeNoopRefCount)
	m.Load(3, &x.InodeNotDirectory)
	m.Load(4, &x.InodeNotSymlink)
	m.Load(5, &x.locks)
	m.Load(6, &x.dentry)
	m.Load(7, &x.root)
	m.Load(8, &x.t)
}

func (x *replicaFileDescription) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.replicaFileDescription"
}

func (x *replicaFileDescription) StateFields() []string {
	return []string{
		"vfsfd",
		"FileDescriptionDefaultImpl",
		"LockFD",
		"inode",
	}
}

func (x *replicaFileDescription) beforeSave() {}

func (x *replicaFileDescription) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.vfsfd)
	m.Save(1, &x.FileDescriptionDefaultImpl)
	m.Save(2, &x.LockFD)
	m.Save(3, &x.inode)
}

func (x *replicaFileDescription) afterLoad() {}

func (x *replicaFileDescription) StateLoad(m state.Source) {
	m.Load(0, &x.vfsfd)
	m.Load(1, &x.FileDescriptionDefaultImpl)
	m.Load(2, &x.LockFD)
	m.Load(3, &x.inode)
}

func (x *rootInodeRefs) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.rootInodeRefs"
}

func (x *rootInodeRefs) StateFields() []string {
	return []string{
		"refCount",
	}
}

func (x *rootInodeRefs) beforeSave() {}

func (x *rootInodeRefs) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.refCount)
}

func (x *rootInodeRefs) afterLoad() {}

func (x *rootInodeRefs) StateLoad(m state.Source) {
	m.Load(0, &x.refCount)
}

func (x *Terminal) StateTypeName() string {
	return "pkg/sentry/fsimpl/devpts.Terminal"
}

func (x *Terminal) StateFields() []string {
	return []string{
		"n",
		"ld",
		"masterKTTY",
		"replicaKTTY",
	}
}

func (x *Terminal) beforeSave() {}

func (x *Terminal) StateSave(m state.Sink) {
	x.beforeSave()
	m.Save(0, &x.n)
	m.Save(1, &x.ld)
	m.Save(2, &x.masterKTTY)
	m.Save(3, &x.replicaKTTY)
}

func (x *Terminal) afterLoad() {}

func (x *Terminal) StateLoad(m state.Source) {
	m.Load(0, &x.n)
	m.Load(1, &x.ld)
	m.Load(2, &x.masterKTTY)
	m.Load(3, &x.replicaKTTY)
}

func init() {
	state.Register((*FilesystemType)(nil))
	state.Register((*filesystem)(nil))
	state.Register((*rootInode)(nil))
	state.Register((*implStatFS)(nil))
	state.Register((*lineDiscipline)(nil))
	state.Register((*outputQueueTransformer)(nil))
	state.Register((*inputQueueTransformer)(nil))
	state.Register((*masterInode)(nil))
	state.Register((*masterFileDescription)(nil))
	state.Register((*queue)(nil))
	state.Register((*replicaInode)(nil))
	state.Register((*replicaFileDescription)(nil))
	state.Register((*rootInodeRefs)(nil))
	state.Register((*Terminal)(nil))
}
