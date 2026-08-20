"""Network constants for the warp.green bridge (Base -> Chia).

Values are transcribed verbatim from the warp-ui ``config.tsx`` deployment
config (validator BLS G1 keys, multisig keys, EVM validator addresses, Nostr
relays + x-only keys, thresholds, contract addresses, launcher ids) plus the
watcher/status endpoints. The BLS validator keys are needed regardless of Nostr
because currying the portal-receiver inner puzzle commits to them.

**Mainnet only.** A Base Sepolia / testnet11 deployment used to live here too,
but it could never actually bridge (Base Sepolia has no wired USDC, and the EVM
client only builds ERC-20 ``approve``/``bridgeToChia`` calls) *and* it shipped
with an empty :attr:`WarpNet.expected_asset_id`, which disabled the one anchor
that refuses to move funds. It was a rehearsal that exercised less safety than
production, so it was removed. Rehearse with ``warp.dry_run: true`` instead --
that signs both Base transactions and stops before the broadcast.

Correctness anchors live here too: :attr:`WarpNet.expected_asset_id` (the
wrapped-TAIL hash the derivation must reproduce before any funds move) and
:attr:`WarpNet.bridging_puzzle_hash`. See ``tests/test_warp_clvm_anchors.py``.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple


@dataclass(frozen=True)
class WarpNet:
    """Immutable snapshot of the warp.green mainnet deployment."""

    name: str
    source_chain: str  # warp chain id of the EVM source, "bse" for Base

    # --- Chia / coinset ---------------------------------------------------- #
    coinset_url: str
    chia_prefix: str  # bech32m HRP: "xch"
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
    usdc_address: str  # the Base USDC ERC-20; always set
    # warp.green's MilliETH wrapper on Base: deposit() mints 1000 milliETH
    # (3 decimals) per ETH, withdraw() burns back to ETH, no fees; ETH
    # amounts must be multiples of 1e12 wei (the 6-decimal conversion).
    # Address verified against docs.warp.green's Base deployments table
    # 2026-08-19 (same table that anchors portal_address and
    # erc20_bridge_address above).
    milli_eth_address: str
    milli_eth_decimals: int  # 3: smallest unit = 0.001 milliETH = 1e12 wei
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
    expected_asset_id: str  # wUSDC.b wrapped-TAIL hash (hard anchor; see assets)
    bridging_puzzle_hash: str  # network-independent; a09eb1ea...9037

    # --- outbound (unwrap) ------------------------------------------------- #
    # The Chia-side message toll: the cat_burner coin's amount, burned as the
    # bundle's RESERVE_FEE (it doubles as the mempool fee -- design doc §4).
    # Also the burn inner puzzle's curried BRIDGE_FEE, proven by evaluating the
    # vendored bytecode against a real mainnet unwrap.
    chia_toll_mojos: int
    # sha256tree(get_cat_burner_puzzle(b"bse", erc20_bridge)) -- derived
    # offline in the anchor tests AND read live from ERC20Bridge.burnPuzzleHash()
    # at gate time. This is ALSO the outbound message's `source` field: the
    # validators attest the cat_burner PUZZLE HASH, not any coin id.
    burn_puzzle_hash: str
    # EIP-712 domain fields, read live via eip712Domain() (EIP-5267) at gate
    # time and asserted against these. The three separator getters
    # (domainSeparator() and friends) all revert on the deployed Portal, so
    # EIP-5267 is the only live read -- proven by execution.
    eip712_name: str
    eip712_version: str
    # The reconstructed separator, anchored in tests; binds chainId 8453 and
    # the portal address, so a redeploy changes it and the gate fails closed.
    eip712_domain_separator: str

    # --- bridgeable assets -------------------------------------------------- #
    # Every ERC-20 the pipeline may bridge, keyed by symbol. USDC's entry
    # mirrors the legacy scalars (usdc_address / usdc_decimals /
    # expected_asset_id), which remain during the migration because ~10 call
    # sites still read them; new code reads this table. Adding an asset here
    # requires its offline-derived wrapped-TAIL anchor -- the engine refuses
    # to build if any listed asset's derivation disagrees.
    assets: "Tuple[Tuple[str, WarpAsset], ...]" = ()

    def asset(self, symbol: str) -> "WarpAsset":
        """The descriptor for ``symbol``; KeyError on unknown assets."""
        for key, spec in self.assets:
            if key == symbol:
                return spec
        raise KeyError(f"{self.name} has no bridgeable asset {symbol!r}")


@dataclass(frozen=True)
class WarpAsset:
    """One bridgeable ERC-20 and its wrapped-CAT identity, pinned offline.

    ``expected_asset_id`` is the sha256tree of the wrapped TAIL derived from
    (portal launcher, source chain, ERC20Bridge, ``erc20_address``) -- the
    same derivation :func:`drivers.derive_wrapped_asset_id` performs, and the
    same anchor discipline as :attr:`WarpNet.expected_asset_id`: a wrong or
    missing value must refuse to run, never fall back. The engine verifies
    EVERY listed asset's derivation at construction, so a typo here is a
    blocked engine with a banner, not a claim minting an untradeable CAT.
    """

    symbol: str
    erc20_address: str
    erc20_decimals: int
    # sha256tree(wrapped TAIL); pinned offline, verified at engine build.
    expected_asset_id: str


MAINNET = WarpNet(
    name="mainnet",
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
    milli_eth_address="0xf2D5d8eC69E2faed5eB4De90749c87ee314a4B12",
    milli_eth_decimals=3,
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
    assets=(
        ("USDC", WarpAsset(
            symbol="USDC",
            # Exact-casing mirror of usdc_address above (anchored by test).
            erc20_address="0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
            erc20_decimals=6,
            expected_asset_id="fa4a180ac326e67ea289b869e3448256f6af05721f7cf934"
                              "cb9901baa6b7a99d",
        )),
        # wmilliETH.b: the SAME CAT the enabled wmilliETH.b/XCH trading pair
        # quotes (config.yaml base_asset_id), so bridged tokens land directly
        # as ask-side inventory. Derived offline from milli_eth_address on
        # 2026-08-19 and pinned; the anchor test re-derives it.
        ("milliETH", WarpAsset(
            symbol="milliETH",
            erc20_address="0xf2D5d8eC69E2faed5eB4De90749c87ee314a4B12",
            erc20_decimals=3,
            expected_asset_id="f322a205c034fe28681829fa5a2e483ac421f0952eb129"
                              "2945c8db06e0a471a6",
        )),
    ),
    bridging_puzzle_hash="a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037",
    chia_toll_mojos=1_000_000_000,  # 0.001 XCH, both real mainnet unwraps
    burn_puzzle_hash="6d64cf902916f73b90fa0a6412c7d1b43996c04fb3f245fcc2d767aa556c93a1",
    eip712_name="warp.green Portal",
    eip712_version="1",
    eip712_domain_separator="9a1b113816cfc7af201f437d8ddea6c268c09303485630f6b38e9dbd28bd57eb",
)
