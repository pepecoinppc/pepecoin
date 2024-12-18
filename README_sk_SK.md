<h1 align="center">
<img src="https://i.imgur.com/DDkfI9i.png" alt="Pepecoin" width="300"/>
<br/><br/>
Pepecoin Core [PEPE, Ᵽ]
</h1>

Vyberte jazyk: [EN](./README.md) | [CN](./README_zh_CN.md) | [PT](./README_pt_BR.md) | [FA](./README_fa_IR.md) | [VI](./README_vi_VN.md) | [FR](./README_fr_FR.md) | [JA](./README_ja_JP.md) | [DE](./README_de_DE.md) | SK

Pepecoin je komunitou orientovaná kryptomena vytvorená jedným z pôvodných "Dogecoin shibes" z roku 2013. Bola vytvorená s jediným cieľom – vytvoriť novú a zábavnú komunitu podobnú pôvodnej Dogecoin komunite.

Na rozdiel od všetkých predošlých iterácií je Pepecoin "Layer 1" mincou. To znamená, že neexistujú žiadne "liquidity pools" na vyčerpanie, žiadne čierne listiny peňaženiek a žiadne mätúce "smart contracts". Pepecoin je jednoduchý blockchain.

Softvér Pepecoin Core umožňuje komukoľvek prevádzkovať uzol v sieti blockchainu Pepecoin a využíva metódu Scrypt hashovania na Proof of Work tažbu. Je adaptovaný z Dogecoin Core, Bitcoin Core a iných kryptomien.

Pre informácie o predvolených poplatkoch transakcií používaných v sieti Pepecoin si prečítajte [odporúčania o poplatkoch](doc/fee-recommendation.md).

**Webová stránka:** [pepecoin.org](https://pepecoin.org)

## Rozdiely oproti Dogecoinu

Pepecoin je fork Dogecoin-u. Z dôvodu známosti Dogecoin-u sa budeme snažiť zachovať Pepecoin podobný Dogecoin-u.

Zmeny:

* Adresy začínajú na `P` namiesto `D`.
* Funkcie BIPS začnú od bloku 1000.
* AuxPow začína od bloku 42,000 (Chain ID: 63).
* GUI je prispôsobené pre Pepecoin.

## Použitie 💻

Ak chcete začať svoju cestu s Pepecoin Core, pozrite si [príručku na inštaláciu](INSTALL.md) a [návod na začiatok](doc/getting-started.md).

JSON-RPC API poskytované Pepecoin Core sa dá sa prehliadať pomocou príkazu `pepecoin-cli help`. Podrobné informácie pre jednotlivé príkazy získate pomocou `pepecoin-cli help <command>`. Alternatívne si pozrite [dokumentáciu Bitcoin Core](https://developer.bitcoin.org/reference/rpc/), ktorá implementuje podobný protokol.

### Porty

Pepecoin Core predvolene používa port `33874` na peer-to-peer komunikáciu, ktorá je potrebná na synchronizáciu blockchainu "mainnet" a na prijímanie nových transakcií a blokov. Okrem toho môže byť otvorený port JSONRPC, ktorý predvolene používa port `33873` pre uzly mainnet. Dôrazne sa odporúča neotvárať RPC porty pre verejný internet.

| Funkcia | mainnet | testnet | regtest |
| :------ | ------: | ------: | ------: |
| P2P     |   33874 |   44874 |   18444 |
| RPC     |   33873 |   44873 |   18332 |

## Prebiehajúci vývoj 💻

Pepecoin Core je open-source softvér riadený komunitou. Vývojový proces je otvorený a verejne viditeľný; ktokoľvek môže vidieť, diskutovať a pracovať na softvéri.

Hlavné zdroje vývoja:

* [GitHub Projects](https://github.com/pepecoinppc/pepecoin/projects) sa používa na sledovanie plánovanej a prebiehajúcej práce na nadchádzajúcich vydaniach.
* [GitHub Discussion](https://github.com/pepecoinppc/pepecoin/discussions) sa používa na diskusiu o funkciách, plánovaných aj neplánovaných, ktoré súvisia s vývojom softvéru Pepecoin Core, základnými protokolmi a aktívom PEPE.
* [PepecoinDev subreddit](https://www.reddit.com/r/pepecoindev/)

### Strategia verzií
Čísla verzií sledujú schému ```major.minor.patch```.

### Vetvy
V tomto repozitári existujú 3 typy vetiev:

- **master:** Stabilná, obsahuje najnovšiu verziu posledného *major.minor* vydania.
- **maintenance:** Stabilná, obsahuje najnovšiu verziu predchádzajúcich vydaní, ktoré sú stále aktívne udržiavané. Formát: ```<verzia>-maint```
- **development:** Nestabilná, obsahuje nový kód pre plánované vydania. Formát: ```<verzia>-dev```

*Vetvy master a maintenance sú výhradne mutované len pri vydaní. Plánované*
*vydania budú vždy mať vývojovú vetvu a pull requesty by mali byť podávané voči týmto vetvám. Maintenance vetvy slúžia výhradne na **opravy chýb***
*Nové funkcie prosím podávajte voči vývojovej vetve s najvyššou verziou.*

## Prispievanie 🤝

Ak nájdete chybu alebo narazíte na problém s týmto softvérom, nahláste ho
pomocou [systému nahlasovania chýb](https://github.com/pepecoinppc/pepecoin/issues/new?assignees=&labels=bug&template=bug_report.md&title=%5Bbug%5D+).

Pozrite si [príručku pre prispievateľov](CONTRIBUTING.md), aby ste zistili, ako sa môžete
zapojiť do vývoja Pepecoin Core. Skúste [pomôcť ostatným](https://github.com/pepecoinppc/pepecoin/labels/help%20wanted) s ich problémami, kde vaše príspevky môžu mať veľký vplyv a budú veľmi ocenené.

## Komunity 🐸

Môžete sa pripojiť k komunitám na rôznych sociálnych médiách.
Sledujte aktuálne dianie, stretávajte ľudí, diskutujte, nájdite najnovšie mémi, učte sa
o Pepecoine, dávajte alebo žiadajte o pomoc, zdieľajte svoj projekt.

Tu sú niektoré miesta, ktoré môžete navštíviť:

* [r/Pepecoin](https://www.reddit.com/r/pepecoin/)
* [Discord](https://pepecoin.org/discord)
* [Telegram](https://t.me/PepecoinGroup)
* [Twitter/X](https://twitter.com/PepecoinNetwork)

## Často kladené otázky ❓

Máte otázku ohľadom Pepecoinu? Možno už je zodpovedaná v [FAQ](doc/FAQ.md) alebo v [sekcii otázok a odpovedí](https://github.com/pepecoinppc/pepecoin/discussions/categories/q-a) na diskusnom fóre!

## Licencia ⚖️
Pepecoin Core je vydaný pod licenciou MIT. Viac informácií nájdete v súbore
[COPYING](COPYING) alebo na stránke [opensource.org](https://opensource.org/licenses/MIT)
