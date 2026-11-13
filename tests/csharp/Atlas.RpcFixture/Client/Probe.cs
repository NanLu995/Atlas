using Atlas.Client;

namespace Atlas.RpcFixture.Client;

[Atlas.Entity.Entity("Probe")]
public partial class Probe : ClientEntity
{
    public int LastNotice { get; private set; }

    public partial void Notice(int value) => LastNotice = value;
}
