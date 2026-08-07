"""Network constants for the warp.green bridge (Base <-> Chia).

Values are transcribed verbatim from the warp-ui ``config.tsx`` deployment
config (validator BLS G1 keys, multisig keys, EVM validator addresses, Nostr
relays + x-only keys, thresholds, contract addresses, launcher ids) plus the
watcher/status endpoints. The BLS validator keys are needed regardless of Nostr
because currying the portal-receiver inner puzzle commits to them.

Two frozen instances are provided: :data:`MAINNET` (Base + Chia mainnet) and
:data:`TESTNET` (Base Sepolia + testnet11). Select with :func:`net_for`.

Correctness anchors live here too: :attr:`WarpNet.expected_asset_id` (the
wrapped-TAIL hash the derivation must reproduce before any funds move) and
:attr:`WarpNet.bridging_puzzle_hash`. See ``tests/test_warp_clvm_anchors.py``.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple


@dataclass(frozen=True)
class WarpNet:
    """Immutable snapshot of one warp.green deployment (mainnet or testnet)."""

    name: str
    testnet: bool
    source_chain: str  # warp chain id of the EVM source, "bse" for Base

    # --- Chia / coinset ---------------------------------------------------- #
    coinset_url: str
    chia_prefix: str  # bech32m HRP: "xch" | "txch"
    portal_launcher_id: str  # portal singleton launcher coin id (hex)
    agg_sig_extra_data: str  # AGG_SIG_ME additional data (genesis challenge, hex)
    signature_threshold: int  # validator BLS sigs required to claim (m of n)
    multisig_threshold: int  # portal-updater multisig threshold
    chia_confirmation_min_height: int
    validator_bls_keys: Tuple[str, ...]  # G1 pubkeys, index-ordered (11: 10 + inf pad)
    multisig_bls_keys: Tuple[str, ...]  # G1 pubkeys of the portal updater multisig
    chia_explorer_url: str

    # --- EVM / Base -------------------------------------------------------- #
    evm_chain_id: int
    evm_default_rpc_url: str
    portal_address: str
    erc20_bridge_address: str
    usdc_address: str  # "" when the network has no wired USDC (Base Sepolia)
    usdc_decimals: int
    cat_decimals: int  # wrapped CAT precision (3 on Chia); 1 USDC unit / 10**(6-3) mojos
    evm_confirmation_min_height: int
    message_toll_wei: int  # Portal.messageToll(); owner-mutable, always read live
    evm_validator_addresses: Tuple[str, ...]  # checksummed, index-ordered (10)
    base_explorer_url: str

    # --- Nostr ------------------------------------------------------------- #
    nostr_relays: Tuple[str, ...]
    nostr_validator_keys: Tuple[str, ...]  # x-only pubkeys, index-ordered (10)
    watcher_api_url: str
    status_url: str

    # --- correctness anchors ---------------------------------------------- #
    expected_asset_id: str  # wrapped-TAIL hash; "" disables the anchor (testnet)
    bridging_puzzle_hash: str  # network-independent; a09eb1ea...9037


MAINNET = WarpNet(
    name="mainnet",
    testnet=False,
    source_chain="bse",
    # Chia mainnet
    coinset_url="https://api.coinset.org",
    chia_prefix="xch",
    portal_launcher_id="46e2bdbbcd1e372523ad4cd3c9cf4b372c389733c71bb23450f715ba5aa56d50",
    agg_sig_extra_data="ccd5bb71183532bff220ba46c268991a3ff07eb358e8255a65c30a2dce0e5fbb",
    signature_threshold=6,
    multisig_threshold=6,
    chia_confirmation_min_height=32,
    validator_bls_keys=(
        "8d7b289831084afb41ec99d4ccd781b0a7e5c01fb9a1d3a6e0af70582bb2c7ff3bc36d657e7bfba60e5119d62bd30993",
        "b4c92890f9dfdf47d674943a8acbdb4b695c6936d79c66c62a5679c2dc2fe649ba11431f474ff0b168cf709515e0b6e6",
        "a8fcd9e4afef11b5e7072dc418849189bf14c75b40d9cb5b2f5b6657c27f1a119381304b7bc4fe7aad33ced25f63bf76",
        "989eaa2133912b060485b633ff2ab9063788472857514970fa521407fd4ac36f65aa35e723c736f5f77f36fed0f7064c",
        "8b9d5443a67c74229427c2f917138ec23032892df8302d209f25e8f8ff8192303d82dbab603b50e207da94887fc8c446",
        "b59977c6144eb5e666c7276fda1bf15a1ee23ca57f63487ae8c5694d69a7eac24ed3036d8562749f4586f10a2d51f3eb",
        "ada250cbdf981a00cc69fa24e326d5528e6f7d3d5283ee8e788ae018ddc511cc4bde6e7de0919c5e37ac5af97f47ba35",
        "abb4408a3e0c9cfb14f2b0bd27265a7650c3d1d916e9ac3ef8d2a91558364ce4610575c7339767d1ffdd52ffff085d50",
        "8efc3b506c75e91a66357f9b965721434074ea148fd54d25dce8371a8496f9f66f6bb811fafa73d77798eb9e02d0bf7f",
        "a166b08281b6f29d858e1723a85ca4581740b09ad3f38f35db5038ebd06162cf45d41d13acb290b392db3b10dbb230eb",
        "c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    ),
    multisig_bls_keys=(
        "b63871fbc72a7ff07d8f2419c8d3bbe1ac557d3cbf367761d08a1a1209dd285358124845151381da912751e33bd7ffa8",
        "8d5ca1a64a587c2fe7603a6933d335ad01a50f0187085981651d617ffaffce9d57ad25680813a030665fabef12075811",
        "aa5ea815c1c0e70882b532bb7462a2d8ba68817a4ccfb728214382217b671fd534e19b869ad404ecd5c7852520c6f0c0",
        "b2ee00a2e657ee8bb818999f56ec781c4b10a04919eb061904e44bc96f022e47a75454b1d63796ea6eff8fcf2932d8ca",
        "9322d4a1f8d078b81ea674947ba2420f4175f38483d7ac60dc3ff4de3d27cf33bb1c06cb7638467536ef766533a7ad79",
        "a7b9970795c085979ca94b54e5c1b8e4ee96104dac690c01175b138b327c85e2537e53dc97189ecca57b629d3283bdca",
        "813dcc8c4870df68416f14253a559aa0e088db84a46b9fe3e442eb6dbc2a1cb5381e2e862927a2743a1903889aadad1b",
        "b5bd04adb90273d97a458b5e42d4930ab35643203131c22d53ac312026da74fda64c71216fc6db263063262266c45727",
        "84b00e171a571b5904e48cdf456bf2861d37f01961c8b641a66974c70f49393fc55ba4b05543ca2894e8f6c7daad0719",
        "836324ba44d7e1f2290a1ccf4c3c2d064c5047619981198ec4059165d85c06a1fd214da2b1b58603254f9b48637a9db1",
        "c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    ),
    chia_explorer_url="https://spacescan.io/",
    # Base mainnet
    evm_chain_id=8453,
    evm_default_rpc_url="https://mainnet.base.org",
    portal_address="0x382bd36d1dE6Fe0a3D9943004D3ca5Ee389627EE",
    erc20_bridge_address="0x8412f06e811b858Ea9edcf81a5E5882dbf70aC96",
    usdc_address="0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
    usdc_decimals=6,
    cat_decimals=3,
    evm_confirmation_min_height=64,
    message_toll_wei=10_000_000_000_000,  # parseEther("0.00001")
    evm_validator_addresses=(
        "0x12a67BDC9a74dc0Bde185d6cA03480a16BFB0E96",
        "0x0838a3f6B6465BF44898c91B89823B4D743001Cb",
        "0x9b03A7e2868B922D0f24bedC63145EDb04697A60",
        "0xDd0f7b677cD79A28Faf43A1140251fd804341943",
        "0xCEc9e92B3C9D7fd7f8211FB8CaD24ba064A9185c",
        "0x9EC3559492Cd4F1109EE6467B052184F79C28fe7",
        "0xe456b36224f163242778db6C877eaED81922166F",
        "0xAd2169657d32B302a6519C545B5425608e4aC4E2",
        "0x8094548A72eadAC2742F368E9e8Bf644FF17D03f",
        "0x5110FB4762021ad3954Bdf2caBF4510C0ACd6d2f",
    ),
    base_explorer_url="https://basescan.org",
    # Nostr mainnet
    nostr_relays=(
        "wss://relay.fireacademy.io",
        "wss://relay.bufflehead.org",
        "wss://xch-relay.tns.cx",
        "wss://relay.spacescan.io",
        "wss://relay.chainhq.tech",
        "wss://relay.ozonewallet.io",
        "wss://warpgreen-relay.232220.xyz",
        "wss://relay.msmc.dev",
        "wss://warpgreen-mainnet-relay.midl.dev",
        "wss://relay.giritec.com",
    ),
    nostr_validator_keys=(
        "db5790fd1aac8f0cb60879cd468b0cc845e5b692350ef7a26d4776c4f6da3776",
        "ad4bc8487872b07d5acd9dd4ee11906e107a97945f2141eb60d6f0880c29f8e7",
        "85146a6d0a14a2ae1e8eaa27142f7880caf5fe4428e11fb1fcdc0dc010a8829a",
        "ca0085b5cae15bcc80740bb62ab3688cee8fc88dd9520edf23ce120217e653e5",
        "5e9a145844238c5968c79a86fa614acc79edd1628e2267e06495a0b2e4aab7ba",
        "7eff2950197deca52b67901a9641f4e4aac84b8bdd973d44edc6d73fb98af259",
        "7567a34d43e5fbed05afe8b085eadf2462a9f9cf8e1bbb53701ecb9e04e8c09c",
        "10eade3fefcf87d15235bf23e9e6c23bef85aac2762badabe18567fe603c1945",
        "e0a2e65ee292aff65b0fa92a74541a4e5b54f2919bfa6dba08e7df25b4300fb6",
        "2239f413ce7b399ad1e91e2fb4742960d73637b87a3616c4a28771cc84fb648e",
    ),
    watcher_api_url="https://watcher-api.warp.green/",
    status_url="https://status.warp.green/",
    # anchors
    expected_asset_id="fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
    bridging_puzzle_hash="a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037",
)


TESTNET = WarpNet(
    name="testnet",
    testnet=True,
    source_chain="bse",
    # Chia testnet11
    coinset_url="https://testnet11.api.coinset.org/",
    chia_prefix="txch",
    portal_launcher_id="eca7eca48e658d45a752d2b31ca47e5683c1c86de7f7cb5c3285ef6ec56e10f5",
    agg_sig_extra_data="37a90eb5185a9c4439a91ddc98bbadce7b4feba060d50116a067de66bf236615",
    signature_threshold=3,
    multisig_threshold=2,
    chia_confirmation_min_height=5,
    validator_bls_keys=(
        "a60bffc4d51fa503ea6f12053a956de4cbb27a343453643e07eacddde06e7262e4fcd32653d61a731407a1d7e2d6ab2c",
        "89f91c357ce08b0cf4b191c1dea75917fa576a9713731f5d6623c28f421b26c6405b97f403f042100534f2546c12ba06",
        "944b6949e5af6bc4ecf37c858a3e1797f777528f58778aac0c2bf2ca7946273e3f2f1e90600f6fd05d68cc5747c8d9b9",
        "97778660463afdee77fe8ff762faceefbd28b25506d26c584d9cd5a8eb77799fe5cc0b4df04c0004285868d5528aaba0",
        "ac639eb21a717dfd51b1a0a84ba6febf0d359e61f960fbfc87796303d9e0f0c5ebb277b1713391adbfb0dbc3e0eb1bbc",
        "b6e28d4b220eed803baf166de100dd4252c088e87884430ac9a6b7cbc77fd07b89e7ed312bd54c1cc4ab27c23bec7add",
        "aadb92c683f0e05bc75a106adfb604e1be75ab58ca10a5fe9f5e95cbd300302f213d63176f69427fbbc54d6146bd4b54",
        "85e07033fd3f31e73e476e21a3723434138a865edeab36e7debaea928f88ce14a05599d2ac5dfc41860b0159e6f7efb3",
        "98fc5a235bd0880fb9f8de1fb57c67fd468ff2b1989edb1925e80ea359579a7668b6b77777b06d69c6ddfffd626407eb",
        "b8322e6079ef33975028b44dae0c632396e2503982e0c8eb73d3b55343e6ffd75e49f1006db7d49b5151438c83cec351",
        "c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    ),
    multisig_bls_keys=(
        "b93c773fd448927ad5a77d543aa9a2043dad8ab9d8a8ac505317d6542ffdb1b6b74e9e85e734b8ca8264de49b6231a38",
        "8a31dd7d168290b7e4ce878fb42db54bdb4fe4b0f55f0c7208b138f96b8329706a44da6adba4e92b5c3bd6251436cd98",
        "aa5ea815c1c0e70882b532bb7462a2d8ba68817a4ccfb728214382217b671fd534e19b869ad404ecd5c7852520c6f0c0",
        "8a8b61dcb5e2a8455e4b5d7782e70f96bc7667bccb4ea4a13492eb12244df2ec30f8785d579d9b0273461510bc0c497e",
        "9322d4a1f8d078b81ea674947ba2420f4175f38483d7ac60dc3ff4de3d27cf33bb1c06cb7638467536ef766533a7ad79",
        "a7b9970795c085979ca94b54e5c1b8e4ee96104dac690c01175b138b327c85e2537e53dc97189ecca57b629d3283bdca",
        "813dcc8c4870df68416f14253a559aa0e088db84a46b9fe3e442eb6dbc2a1cb5381e2e862927a2743a1903889aadad1b",
        "b5bd04adb90273d97a458b5e42d4930ab35643203131c22d53ac312026da74fda64c71216fc6db263063262266c45727",
        "84b00e171a571b5904e48cdf456bf2861d37f01961c8b641a66974c70f49393fc55ba4b05543ca2894e8f6c7daad0719",
        "a0d0dc619cdca2045c2cfde7e7884d60e71c7216a21cb3a327f76767f19516069a6c4552673a48d4b14e2bc46eebc069",
        "c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    ),
    chia_explorer_url="https://testnet11.spacescan.io/",
    # Base Sepolia
    evm_chain_id=84532,
    evm_default_rpc_url="https://sepolia.base.org",
    portal_address="0x1c14e49d74c4c8302CEFC58A08E3CE77Eb38A066",
    erc20_bridge_address="0x1e15a85558042aa1378071853dA500D3A3669214",
    usdc_address="",  # no wired USDC on Base Sepolia; testnet drill bridges ETH
    usdc_decimals=6,
    cat_decimals=3,
    evm_confirmation_min_height=10,
    message_toll_wei=10_000_000_000_000,  # parseEther("0.00001")
    evm_validator_addresses=(
        "0x113f132a978B7679Aa72c02B0234a32569507043",
        "0xC1cF931aE32e7592fa25f5a0978B7663f20aafBd",
        "0x716f7ff65adcbBc68cb5bF34178C87aBC2B08ab6",
        "0xfB347BbC9C21CD71B0D86ffa99d2667372DBCa8B",
        "0x7d325C4783F9F572Fde31Ae358e28Cc3d120Cd8A",
        "0xEeec13CD790C47740DBcbacCA96D189962E3600F",
        "0x144eE8FaB13Eed04b5A8F339105076B8BC51cAF2",
        "0xE42277e12F577810e73f70b42e184dd5BcD30ce6",
        "0x92c02A599E056B2706f74421f4250951E1f736ab",
        "0xF187Be30D8681BF67A6a1D1c77663f0e19Fe2d7d",
    ),
    base_explorer_url="https://sepolia.basescan.org",
    # Nostr testnet
    nostr_relays=(
        "wss://test-relay.fireacademy.io",
        "wss://txch-relay.bufflehead.org",
        "wss://txch-relay.tns.cx",
        "wss://testnet-relay.spacescan.io",
        "wss://relay.prime-tek.com",
        "wss://testrelay.ozonewallet.io",
        "wss://warpgreen-relay-test.232220.xyz",
        "wss://testnet-relay.msmc.dev",
        "wss://warpgreen-testnet-relay.midl.dev",
        "wss://relay.testnet.giritec.com",
    ),
    nostr_validator_keys=(
        "cd5fd0859c3a27c13dd9734b7cdc6f2c25646e45821dcecaa089808803d01706",
        "804f399157512ee115d19e0e15fd63f97f6330755c6f928a8c49a6bda35df74e",
        "6b8d53c62a1d5f9bc68605a7139c06c9ce6d727ecf815044b46cb983fcc17e52",
        "6f2aa84aa51327db3507e3c0d0fd02876d1b53fecf42aa88e9d32bbf5247ad3d",
        "c26ce22c6241420e136b8562f9d89e73553a89f29ee15cb0194dd2be184601b8",
        "6f6edc8adb8034a605a59109037d1bfdbab3ba38b564db767d29c0ecbe399916",
        "280e03462f0f87ba2aefeb7a033c2c1ec134fc9c06f28a5e9ed308dabf9e7700",
        "14aed9c23d7774dda60532891423e4e8adf74f468ea5f5cbac58cf8994387b94",
        "38c4190c961bfbc0a23b2c274c36fd6ccad5d50033e8bb263fc4aa23930ededf",
        "b94fe9fd1a47f33bdf1aa6cbf295734e6fc7fdf2d210403edc8b177801c0a849",
    ),
    watcher_api_url="https://watcher-api.testnet.warp.green/",
    status_url="https://warp-validators.bufflehead.org/",
    # anchors
    expected_asset_id="",  # derived at runtime for the testnet asset; anchor skipped
    bridging_puzzle_hash="a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037",
)


def net_for(testnet: bool) -> WarpNet:
    """Return the :class:`WarpNet` for the requested mode."""
    return TESTNET if testnet else MAINNET
