-- Try to verify that rows end up in the right place.

drop table if exists T;
drop table if exists U;
drop table if exists W;

create table T (a int, b int) distributed by (a);
insert into T select i, 1 from generate_series(1, 5000) i;

create table U(segid int, a int, b int) distributed by (a);
insert into U(segid, a, b) select gp_segment_id, * from T;

select * from U where segid <> gp_segment_id; -- should return 0 rows

-- Hash doesn't work quite like this.
-- (numsegments can come from something like in jetpack.sql's __gp_number_of_segments view).
--select * from T where gp_segment_id <> a % numsegments; -- should return 0 rows

create table W(segid int, a int) distributed by (a);
insert into W(segid, a) select gp_segment_id, a*91 from T;

select * from T full join W on T.a = W.a/91 where T.gp_segment_id <> W.segid; -- should return 0 rows 

drop table T;
drop table U;
drop table W;

drop table if exists customer_off;
drop table if exists customer_on;
CREATE TABLE customer_off (
    c_custkey integer NOT NULL,
    c_name character varying(25) NOT NULL,
    c_address character varying(40) NOT NULL,
    c_nationkey integer NOT NULL,
    c_phone character(15) NOT NULL,
    c_acctbal numeric(15,2) NOT NULL,
    c_mktsegment character(10) NOT NULL,
    c_comment character varying(117) NOT NULL
) distributed by (c_custkey);

CREATE TABLE customer_on (
    c_custkey integer NOT NULL,
    c_name character varying(25) NOT NULL,
    c_address character varying(40) NOT NULL,
    c_nationkey integer NOT NULL,
    c_phone character(15) NOT NULL,
    c_acctbal numeric(15,2) NOT NULL,
    c_mktsegment character(10) NOT NULL,
    c_comment character varying(117) NOT NULL
) distributed by (c_custkey);

set gp_enable_fast_sri=off;

INSERT INTO customer_off VALUES (500, 'Customer#000000500', 'fy7qx5fHLhcbFL93duj9', 4, '14-194-736-4233', 3300.82, 'AUTOMOBILE', 's boost furiously. slyly special deposits sleep quickly above the furiously i');
INSERT INTO customer_off VALUES (502, 'Customer#000000502', 'nouAF6kednGsWEhQYyVpSnnPt', 11, '21-405-590-9919', 1378.67, 'HOUSEHOLD ', 'even asymptotes haggle. final, unusual theodolites haggle. carefully bo');
INSERT INTO customer_off VALUES (504, 'Customer#000000504', '2GuRx4pOLEQWU7fJOa, DYiK8IuMsXRLO5D 0', 10, '20-916-264-7594', 0.51, 'FURNITURE ', 'slyly final theodolites are across the carefully ');
INSERT INTO customer_off VALUES (506, 'Customer#000000506', 'dT kFaJww1B', 13, '23-895-781-8227', 1179.85, 'HOUSEHOLD ', ' idle instructions impress blithely along the carefully unusual notornis. furiously even packages');
INSERT INTO customer_off VALUES (508, 'Customer#000000508', 'q9Vq9 nTrUvx', 18, '28-344-250-3166', 1685.90, 'BUILDING  ', 'uses dazzle since the carefully regular accounts. patterns around the furiously even accounts wake blithely abov');
INSERT INTO customer_off VALUES (510, 'Customer#000000510', 'r6f34uxtNID YBuAXpO94BKyqjkM0qmT5n0Rmd9L', 5, '15-846-260-5139', 1572.48, 'HOUSEHOLD ', 'symptotes. furiously careful re');
INSERT INTO customer_off VALUES (513, 'Customer#000000513', 'sbWV6FIPas6C0puqgnKUI', 1, '11-861-303-6887', 955.37, 'HOUSEHOLD ', 'press along the quickly regular instructions. regular requests against the carefully ironic s');
INSERT INTO customer_off VALUES (515, 'Customer#000000515', 'oXxHtgXP5pXYTh', 15, '25-204-592-4731', 3225.07, 'BUILDING  ', 'ackages cajole furiously special, ironic deposits. carefully even Tiresias according to ');
INSERT INTO customer_off VALUES (517, 'Customer#000000517', 'mSo5eI8F4E6Kgl63nWtU84vfyQjOBg4y', 10, '20-475-741-4234', 3959.71, 'FURNITURE ', 'al, ironic foxes. packages wake according to the pending');
INSERT INTO customer_off VALUES (519, 'Customer#000000519', 'Z6ke6Y9J2pYuPBp7jE', 5, '15-452-860-5592', 9074.45, 'BUILDING  ', 'es. fluffily regular accounts should have to sleep quickly against the carefully ironic foxes. furiously daring');
INSERT INTO customer_off VALUES (521, 'Customer#000000521', 'MUEAEA1ZuvRofNY453Ckr4Apqk1GlOe', 2, '12-539-480-8897', 5830.69, 'MACHINERY ', 'ackages. stealthily even attainments sleep carefull');
INSERT INTO customer_off VALUES (523, 'Customer#000000523', 'sHeOSgsSnJi6pwYSr0v5ugiGhgnx7ZB', 10, '20-638-320-5977', -275.73, 'BUILDING  ', ' fluffily deposits. slyly regular instructions sleep e');
INSERT INTO customer_off VALUES (525, 'Customer#000000525', 'w0pOG5FhH45aYg7mKtHQhAWQKe', 19, '29-365-641-8287', 3931.68, 'AUTOMOBILE', ' blithely bold accounts about the quietl');
INSERT INTO customer_off VALUES (527, 'Customer#000000527', 'giJAUjnTtxX,HXIy0adwwvg,uu5Y3RVP', 13, '23-139-567-9286', 4429.81, 'HOUSEHOLD ', 'ending, ironic instructions. blithely regular deposits about the deposits wake pinto beans. closely silent ');
INSERT INTO customer_off VALUES (529, 'Customer#000000529', 'oGKgweC odpyORKPJ9oxTqzzdlYyFOwXm2F97C', 15, '25-383-240-7326', 9647.58, 'FURNITURE ', ' deposits after the fluffily special foxes integrate carefully blithely dogged dolphins. enticingly bold d');
INSERT INTO customer_off VALUES (531, 'Customer#000000531', 'ceI1iHfAaZ4DVVcm6GU370dAuIEmUW1wxG', 19, '29-151-567-1296', 5342.82, 'HOUSEHOLD ', 'e the brave, pending accounts. pending pinto beans above the ');
INSERT INTO customer_off VALUES (533, 'Customer#000000533', 'mSt8Gj4JqXXeDScn2CB PIrlnhvqxY,w6Ohku', 15, '25-525-957-4486', 5432.77, 'HOUSEHOLD ', 'even dolphins boost furiously among the theodo');
INSERT INTO customer_off VALUES (535, 'Customer#000000535', ',2Y kklprPasEp6DcthUibs', 2, '12-787-866-1808', 2912.80, 'BUILDING  ', 'even dinos breach. fluffily ironic');
INSERT INTO customer_off VALUES (537, 'Customer#000000537', 'wyXvxD,4jc', 10, '20-337-488-6765', 2614.79, 'FURNITURE ', 'e carefully blithely pending platelets. furiously final packages dazzle. ironic foxes wake f');
INSERT INTO customer_off VALUES (539, 'Customer#000000539', 'FoGcDu9llpFiB LELF3rdjaiw RQe1S', 6, '16-166-785-8571', 4390.33, 'HOUSEHOLD ', 'ent instructions. pending patter');
INSERT INTO customer_off VALUES (541, 'Customer#000000541', ',Cris88wkHw4Q0XlCLLYVOAJfkxw', 0, '10-362-308-9442', 1295.54, 'FURNITURE ', 'according to the final platelets. final, busy requests wake blithely across th');
INSERT INTO customer_off VALUES (543, 'Customer#000000543', 'JvbSKX7RG3xuqiKQ93C', 17, '27-972-408-3265', 6089.13, 'AUTOMOBILE', 'l, even theodolites. carefully bold accounts sleep about the sly');
INSERT INTO customer_off VALUES (545, 'Customer#000000545', 'AsYw6k,nDUQcMOpEws', 10, '20-849-123-8918', 7505.33, 'AUTOMOBILE', ' carefully final deposits. slyly unusual pinto beans may wake bold requests. unusual courts alongside ');
INSERT INTO customer_off VALUES (547, 'Customer#000000547', '4h SK3dVkE1tQ0NCh', 22, '32-696-724-2981', 6058.08, 'BUILDING  ', 'y express deposits. slyly ironic deposits nod slyly slyly ironic instructions. carefully quick idea');
INSERT INTO customer_off VALUES (549, 'Customer#000000549', 'v5uqfeHLiL1IELejUDnagWqP5pKWa9LtoemziGV', 24, '34-825-998-8579', 91.53, 'BUILDING  ', 'n asymptotes grow blithely. blithely fluffy deposits boost furiously. busily fu');
INSERT INTO customer_off VALUES (551, 'Customer#000000551', 'holp1DkjYzznatSwjG', 15, '25-209-544-4006', -334.89, 'MACHINERY ', 'y special ideas. slyly ironic foxes wake. regular packages alongside of the deposit');
INSERT INTO customer_off VALUES (553, 'Customer#000000553', '8tTlavJ sT', 4, '14-454-146-3094', 4804.57, 'BUILDING  ', 'ully regular requests are blithely about the express, bold platelets. slyly permanent deposits across the');
INSERT INTO customer_off VALUES (555, 'Customer#000000555', 'chm8jY6TfQ8CEnsvpuL6azNZzkqGcZcO8', 15, '25-548-367-9974', 5486.52, 'BUILDING  ', 'lites are blithely ironic ideas. blithely special pinto beans dazzl');
INSERT INTO customer_off VALUES (557, 'Customer#000000557', 'Nt6FUuDR7v', 15, '25-390-153-6699', 9559.04, 'BUILDING  ', 'furiously pending dolphins use. carefully unusual ideas must have to are carefully. express instructions a');
INSERT INTO customer_off VALUES (559, 'Customer#000000559', 'A3ACFoVbP,gPe xknVJMWC,wmRxb Nmg fWFS,UP', 7, '17-395-429-6655', 5872.94, 'AUTOMOBILE', 'al accounts cajole carefully across the accounts. furiously pending pinto beans across the ');
INSERT INTO customer_off VALUES (561, 'Customer#000000561', 'Z1kPCTbeTqGfdly2Ab9KEdE,jIKW', 18, '28-286-185-3047', 2323.45, 'FURNITURE ', 'across the furiously ironic theodolites. final requests cajole. slowly unusual foxes haggle carefully');
INSERT INTO customer_off VALUES (563, 'Customer#000000563', '2RSC1g7cVd,j23HusdkhdCGmiiE', 12, '22-544-152-1215', 3231.71, 'FURNITURE ', ' pinto beans believe fluffily. excuses wake blithely silent requests. b');
INSERT INTO customer_off VALUES (565, 'Customer#000000565', 'HCBXAou,1eP6Z3IynHFI7XmEBgu27Sx', 4, '14-798-211-2891', 2688.88, 'FURNITURE ', 'e. carefully bold deposits sleep regu');
INSERT INTO customer_off VALUES (567, 'Customer#000000567', 'KNE6mpW69IgTjVN', 21, '31-389-883-3371', 8475.17, 'BUILDING  ', 'blithe, even ideas. fluffily special requests wake. c');
INSERT INTO customer_off VALUES (569, 'Customer#000000569', 'Kk20Q5HiysjcPpMlL6pNUZXXuE', 2, '12-648-567-6776', -795.23, 'MACHINERY ', 'sh. blithely special excuses sleep. blithely ironic accounts slee');
INSERT INTO customer_off VALUES (571, 'Customer#000000571', 'hCrDDrMzGhsa6,5K4rGXQ', 2, '12-115-414-4819', 8993.23, 'HOUSEHOLD ', 'le fluffily. ironic, pending accounts poach quickly iron');
INSERT INTO customer_off VALUES (573, 'Customer#000000573', 'BEluH7it7jUcWqb tNLbMIKjU9hrnL7K', 4, '14-354-826-9743', 2333.96, 'HOUSEHOLD ', 'as. furiously even packages sleep quickly final excu');
INSERT INTO customer_off VALUES (575, 'Customer#000000575', '4K6h0pYH,bg2FS5cYL,qqejhvp7EfTlBjRjeVPkq', 1, '11-980-134-7627', 3652.29, 'BUILDING  ', ' final requests cajole after the ironic, bold instructio');
INSERT INTO customer_off VALUES (577, 'Customer#000000577', 'a73SSq2cip7C8nSzdmmscpZyLCZ7KL', 14, '24-662-826-1317', 7059.15, 'FURNITURE ', 'int furiously. slyly express pin');
INSERT INTO customer_off VALUES (579, 'Customer#000000579', '9ST2x,snyY3s', 0, '10-374-175-6181', 1924.96, 'MACHINERY ', 'ndencies detect slyly fluffil');
INSERT INTO customer_off VALUES (581, 'Customer#000000581', 's9SoN9XeVuCri', 24, '34-415-978-2518', 3242.10, 'MACHINERY ', 'ns. quickly regular pinto beans must sleep fluffily ');
INSERT INTO customer_off VALUES (583, 'Customer#000000583', 'V3i6Gu9,LZtvdnNppXnI2eKQFx0b36WvL,F ', 13, '23-234-625-4041', 3686.07, 'HOUSEHOLD ', ' haggle. regular, regular accounts hinder carefully i');
INSERT INTO customer_off VALUES (585, 'Customer#000000585', 'OAnZOqr6A,,77WC001ck8BAqvJTW6,dRGoRdX', 16, '26-397-693-4170', 7820.26, 'MACHINERY ', 'ickly ironic requests sleep regularly pending requ');
INSERT INTO customer_off VALUES (587, 'Customer#000000587', 'J2UwoJEQzAOTtuBrxGVag9iWSUPTp', 6, '16-585-233-5906', 7077.79, 'AUTOMOBILE', 've the final asymptotes. carefully final deposits wake fu');
INSERT INTO customer_off VALUES (589, 'Customer#000000589', 'TvdYNogIzDfr 1UyJE4b9RTENPmffmIoH', 19, '29-479-316-3576', 1647.05, 'FURNITURE ', 's; blithely ironic theodolites sleep-- accounts haggle around the furiously silent ideas. silent, final packages in');
INSERT INTO customer_off VALUES (591, 'Customer#000000591', 'wGE7AnEtiX7cmCkYA', 20, '30-584-309-7885', 6344.66, 'MACHINERY ', ' regular requests after the deposits cajole blithely ironic pinto beans. platelets about the regular, sp');
INSERT INTO customer_off VALUES (593, 'Customer#000000593', 'SYyEL2nytJXBbFemMseCiivA32USVEDbvGzZS', 9, '19-621-217-1535', 233.51, 'AUTOMOBILE', 've the regular, ironic deposits. requests along the special, regular theodolites lose furi');
INSERT INTO customer_off VALUES (595, 'Customer#000000595', '7Q17BacxM,liY2AwhnHGR0Pjf1180sMz1U', 19, '29-554-215-7805', 4177.17, 'HOUSEHOLD ', 'gular accounts x-ray carefully against the slyl');
INSERT INTO customer_off VALUES (597, 'Customer#000000597', 'Dbv,XVGzl4X', 15, '25-687-952-9485', 2443.52, 'AUTOMOBILE', 'es across the slyly brave packages maintain quickly quickly dogged excuses');
INSERT INTO customer_off VALUES (599, 'Customer#000000599', 'fIvpza0tlXAVjOAPkWN5,DiU0DO4e5NkfgOlXpDI', 4, '14-916-825-6916', 6004.52, 'HOUSEHOLD ', 'thely even requests wake carefully regular theodolites. instructions haggle alongside of the f');

set gp_enable_fast_sri=on;

INSERT INTO customer_on VALUES (500, 'Customer#000000500', 'fy7qx5fHLhcbFL93duj9', 4, '14-194-736-4233', 3300.82, 'AUTOMOBILE', 's boost furiously. slyly special deposits sleep quickly above the furiously i');
INSERT INTO customer_on VALUES (502, 'Customer#000000502', 'nouAF6kednGsWEhQYyVpSnnPt', 11, '21-405-590-9919', 1378.67, 'HOUSEHOLD ', 'even asymptotes haggle. final, unusual theodolites haggle. carefully bo');
INSERT INTO customer_on VALUES (504, 'Customer#000000504', '2GuRx4pOLEQWU7fJOa, DYiK8IuMsXRLO5D 0', 10, '20-916-264-7594', 0.51, 'FURNITURE ', 'slyly final theodolites are across the carefully ');
INSERT INTO customer_on VALUES (506, 'Customer#000000506', 'dT kFaJww1B', 13, '23-895-781-8227', 1179.85, 'HOUSEHOLD ', ' idle instructions impress blithely along the carefully unusual notornis. furiously even packages');
INSERT INTO customer_on VALUES (508, 'Customer#000000508', 'q9Vq9 nTrUvx', 18, '28-344-250-3166', 1685.90, 'BUILDING  ', 'uses dazzle since the carefully regular accounts. patterns around the furiously even accounts wake blithely abov');
INSERT INTO customer_on VALUES (510, 'Customer#000000510', 'r6f34uxtNID YBuAXpO94BKyqjkM0qmT5n0Rmd9L', 5, '15-846-260-5139', 1572.48, 'HOUSEHOLD ', 'symptotes. furiously careful re');
INSERT INTO customer_on VALUES (513, 'Customer#000000513', 'sbWV6FIPas6C0puqgnKUI', 1, '11-861-303-6887', 955.37, 'HOUSEHOLD ', 'press along the quickly regular instructions. regular requests against the carefully ironic s');
INSERT INTO customer_on VALUES (515, 'Customer#000000515', 'oXxHtgXP5pXYTh', 15, '25-204-592-4731', 3225.07, 'BUILDING  ', 'ackages cajole furiously special, ironic deposits. carefully even Tiresias according to ');
INSERT INTO customer_on VALUES (517, 'Customer#000000517', 'mSo5eI8F4E6Kgl63nWtU84vfyQjOBg4y', 10, '20-475-741-4234', 3959.71, 'FURNITURE ', 'al, ironic foxes. packages wake according to the pending');
INSERT INTO customer_on VALUES (519, 'Customer#000000519', 'Z6ke6Y9J2pYuPBp7jE', 5, '15-452-860-5592', 9074.45, 'BUILDING  ', 'es. fluffily regular accounts should have to sleep quickly against the carefully ironic foxes. furiously daring');
INSERT INTO customer_on VALUES (521, 'Customer#000000521', 'MUEAEA1ZuvRofNY453Ckr4Apqk1GlOe', 2, '12-539-480-8897', 5830.69, 'MACHINERY ', 'ackages. stealthily even attainments sleep carefull');
INSERT INTO customer_on VALUES (523, 'Customer#000000523', 'sHeOSgsSnJi6pwYSr0v5ugiGhgnx7ZB', 10, '20-638-320-5977', -275.73, 'BUILDING  ', ' fluffily deposits. slyly regular instructions sleep e');
INSERT INTO customer_on VALUES (525, 'Customer#000000525', 'w0pOG5FhH45aYg7mKtHQhAWQKe', 19, '29-365-641-8287', 3931.68, 'AUTOMOBILE', ' blithely bold accounts about the quietl');
INSERT INTO customer_on VALUES (527, 'Customer#000000527', 'giJAUjnTtxX,HXIy0adwwvg,uu5Y3RVP', 13, '23-139-567-9286', 4429.81, 'HOUSEHOLD ', 'ending, ironic instructions. blithely regular deposits about the deposits wake pinto beans. closely silent ');
INSERT INTO customer_on VALUES (529, 'Customer#000000529', 'oGKgweC odpyORKPJ9oxTqzzdlYyFOwXm2F97C', 15, '25-383-240-7326', 9647.58, 'FURNITURE ', ' deposits after the fluffily special foxes integrate carefully blithely dogged dolphins. enticingly bold d');
INSERT INTO customer_on VALUES (531, 'Customer#000000531', 'ceI1iHfAaZ4DVVcm6GU370dAuIEmUW1wxG', 19, '29-151-567-1296', 5342.82, 'HOUSEHOLD ', 'e the brave, pending accounts. pending pinto beans above the ');
INSERT INTO customer_on VALUES (533, 'Customer#000000533', 'mSt8Gj4JqXXeDScn2CB PIrlnhvqxY,w6Ohku', 15, '25-525-957-4486', 5432.77, 'HOUSEHOLD ', 'even dolphins boost furiously among the theodo');
INSERT INTO customer_on VALUES (535, 'Customer#000000535', ',2Y kklprPasEp6DcthUibs', 2, '12-787-866-1808', 2912.80, 'BUILDING  ', 'even dinos breach. fluffily ironic');
INSERT INTO customer_on VALUES (537, 'Customer#000000537', 'wyXvxD,4jc', 10, '20-337-488-6765', 2614.79, 'FURNITURE ', 'e carefully blithely pending platelets. furiously final packages dazzle. ironic foxes wake f');
INSERT INTO customer_on VALUES (539, 'Customer#000000539', 'FoGcDu9llpFiB LELF3rdjaiw RQe1S', 6, '16-166-785-8571', 4390.33, 'HOUSEHOLD ', 'ent instructions. pending patter');
INSERT INTO customer_on VALUES (541, 'Customer#000000541', ',Cris88wkHw4Q0XlCLLYVOAJfkxw', 0, '10-362-308-9442', 1295.54, 'FURNITURE ', 'according to the final platelets. final, busy requests wake blithely across th');
INSERT INTO customer_on VALUES (543, 'Customer#000000543', 'JvbSKX7RG3xuqiKQ93C', 17, '27-972-408-3265', 6089.13, 'AUTOMOBILE', 'l, even theodolites. carefully bold accounts sleep about the sly');
INSERT INTO customer_on VALUES (545, 'Customer#000000545', 'AsYw6k,nDUQcMOpEws', 10, '20-849-123-8918', 7505.33, 'AUTOMOBILE', ' carefully final deposits. slyly unusual pinto beans may wake bold requests. unusual courts alongside ');
INSERT INTO customer_on VALUES (547, 'Customer#000000547', '4h SK3dVkE1tQ0NCh', 22, '32-696-724-2981', 6058.08, 'BUILDING  ', 'y express deposits. slyly ironic deposits nod slyly slyly ironic instructions. carefully quick idea');
INSERT INTO customer_on VALUES (549, 'Customer#000000549', 'v5uqfeHLiL1IELejUDnagWqP5pKWa9LtoemziGV', 24, '34-825-998-8579', 91.53, 'BUILDING  ', 'n asymptotes grow blithely. blithely fluffy deposits boost furiously. busily fu');
INSERT INTO customer_on VALUES (551, 'Customer#000000551', 'holp1DkjYzznatSwjG', 15, '25-209-544-4006', -334.89, 'MACHINERY ', 'y special ideas. slyly ironic foxes wake. regular packages alongside of the deposit');
INSERT INTO customer_on VALUES (553, 'Customer#000000553', '8tTlavJ sT', 4, '14-454-146-3094', 4804.57, 'BUILDING  ', 'ully regular requests are blithely about the express, bold platelets. slyly permanent deposits across the');
INSERT INTO customer_on VALUES (555, 'Customer#000000555', 'chm8jY6TfQ8CEnsvpuL6azNZzkqGcZcO8', 15, '25-548-367-9974', 5486.52, 'BUILDING  ', 'lites are blithely ironic ideas. blithely special pinto beans dazzl');
INSERT INTO customer_on VALUES (557, 'Customer#000000557', 'Nt6FUuDR7v', 15, '25-390-153-6699', 9559.04, 'BUILDING  ', 'furiously pending dolphins use. carefully unusual ideas must have to are carefully. express instructions a');
INSERT INTO customer_on VALUES (559, 'Customer#000000559', 'A3ACFoVbP,gPe xknVJMWC,wmRxb Nmg fWFS,UP', 7, '17-395-429-6655', 5872.94, 'AUTOMOBILE', 'al accounts cajole carefully across the accounts. furiously pending pinto beans across the ');
INSERT INTO customer_on VALUES (561, 'Customer#000000561', 'Z1kPCTbeTqGfdly2Ab9KEdE,jIKW', 18, '28-286-185-3047', 2323.45, 'FURNITURE ', 'across the furiously ironic theodolites. final requests cajole. slowly unusual foxes haggle carefully');
INSERT INTO customer_on VALUES (563, 'Customer#000000563', '2RSC1g7cVd,j23HusdkhdCGmiiE', 12, '22-544-152-1215', 3231.71, 'FURNITURE ', ' pinto beans believe fluffily. excuses wake blithely silent requests. b');
INSERT INTO customer_on VALUES (565, 'Customer#000000565', 'HCBXAou,1eP6Z3IynHFI7XmEBgu27Sx', 4, '14-798-211-2891', 2688.88, 'FURNITURE ', 'e. carefully bold deposits sleep regu');
INSERT INTO customer_on VALUES (567, 'Customer#000000567', 'KNE6mpW69IgTjVN', 21, '31-389-883-3371', 8475.17, 'BUILDING  ', 'blithe, even ideas. fluffily special requests wake. c');
INSERT INTO customer_on VALUES (569, 'Customer#000000569', 'Kk20Q5HiysjcPpMlL6pNUZXXuE', 2, '12-648-567-6776', -795.23, 'MACHINERY ', 'sh. blithely special excuses sleep. blithely ironic accounts slee');
INSERT INTO customer_on VALUES (571, 'Customer#000000571', 'hCrDDrMzGhsa6,5K4rGXQ', 2, '12-115-414-4819', 8993.23, 'HOUSEHOLD ', 'le fluffily. ironic, pending accounts poach quickly iron');
INSERT INTO customer_on VALUES (573, 'Customer#000000573', 'BEluH7it7jUcWqb tNLbMIKjU9hrnL7K', 4, '14-354-826-9743', 2333.96, 'HOUSEHOLD ', 'as. furiously even packages sleep quickly final excu');
INSERT INTO customer_on VALUES (575, 'Customer#000000575', '4K6h0pYH,bg2FS5cYL,qqejhvp7EfTlBjRjeVPkq', 1, '11-980-134-7627', 3652.29, 'BUILDING  ', ' final requests cajole after the ironic, bold instructio');
INSERT INTO customer_on VALUES (577, 'Customer#000000577', 'a73SSq2cip7C8nSzdmmscpZyLCZ7KL', 14, '24-662-826-1317', 7059.15, 'FURNITURE ', 'int furiously. slyly express pin');
INSERT INTO customer_on VALUES (579, 'Customer#000000579', '9ST2x,snyY3s', 0, '10-374-175-6181', 1924.96, 'MACHINERY ', 'ndencies detect slyly fluffil');
INSERT INTO customer_on VALUES (581, 'Customer#000000581', 's9SoN9XeVuCri', 24, '34-415-978-2518', 3242.10, 'MACHINERY ', 'ns. quickly regular pinto beans must sleep fluffily ');
INSERT INTO customer_on VALUES (583, 'Customer#000000583', 'V3i6Gu9,LZtvdnNppXnI2eKQFx0b36WvL,F ', 13, '23-234-625-4041', 3686.07, 'HOUSEHOLD ', ' haggle. regular, regular accounts hinder carefully i');
INSERT INTO customer_on VALUES (585, 'Customer#000000585', 'OAnZOqr6A,,77WC001ck8BAqvJTW6,dRGoRdX', 16, '26-397-693-4170', 7820.26, 'MACHINERY ', 'ickly ironic requests sleep regularly pending requ');
INSERT INTO customer_on VALUES (587, 'Customer#000000587', 'J2UwoJEQzAOTtuBrxGVag9iWSUPTp', 6, '16-585-233-5906', 7077.79, 'AUTOMOBILE', 've the final asymptotes. carefully final deposits wake fu');
INSERT INTO customer_on VALUES (589, 'Customer#000000589', 'TvdYNogIzDfr 1UyJE4b9RTENPmffmIoH', 19, '29-479-316-3576', 1647.05, 'FURNITURE ', 's; blithely ironic theodolites sleep-- accounts haggle around the furiously silent ideas. silent, final packages in');
INSERT INTO customer_on VALUES (591, 'Customer#000000591', 'wGE7AnEtiX7cmCkYA', 20, '30-584-309-7885', 6344.66, 'MACHINERY ', ' regular requests after the deposits cajole blithely ironic pinto beans. platelets about the regular, sp');
INSERT INTO customer_on VALUES (593, 'Customer#000000593', 'SYyEL2nytJXBbFemMseCiivA32USVEDbvGzZS', 9, '19-621-217-1535', 233.51, 'AUTOMOBILE', 've the regular, ironic deposits. requests along the special, regular theodolites lose furi');
INSERT INTO customer_on VALUES (595, 'Customer#000000595', '7Q17BacxM,liY2AwhnHGR0Pjf1180sMz1U', 19, '29-554-215-7805', 4177.17, 'HOUSEHOLD ', 'gular accounts x-ray carefully against the slyl');
INSERT INTO customer_on VALUES (597, 'Customer#000000597', 'Dbv,XVGzl4X', 15, '25-687-952-9485', 2443.52, 'AUTOMOBILE', 'es across the slyly brave packages maintain quickly quickly dogged excuses');
INSERT INTO customer_on VALUES (599, 'Customer#000000599', 'fIvpza0tlXAVjOAPkWN5,DiU0DO4e5NkfgOlXpDI', 4, '14-916-825-6916', 6004.52, 'HOUSEHOLD ', 'thely even requests wake carefully regular theodolites. instructions haggle alongside of the f');

SELECT * from customer_off a, customer_off b where a.gp_segment_id <> b.gp_segment_id and a.c_custkey = b.c_custkey;

drop table if exists customer_off;
drop table if exists customer_on;
CREATE TABLE customer_off (
    c_custkey integer NOT NULL,
    c_name character varying(25) NOT NULL,
    c_address character varying(40) NOT NULL,
    c_nationkey integer NOT NULL,
    c_phone character(15) NOT NULL,
    c_acctbal numeric(15,2) NOT NULL,
    c_mktsegment character(10) NOT NULL,
    c_comment character varying(117) NOT NULL
) distributed by (c_comment);

CREATE TABLE customer_on (
    c_custkey integer NOT NULL,
    c_name character varying(25) NOT NULL,
    c_address character varying(40) NOT NULL,
    c_nationkey integer NOT NULL,
    c_phone character(15) NOT NULL,
    c_acctbal numeric(15,2) NOT NULL,
    c_mktsegment character(10) NOT NULL,
    c_comment character varying(117) NOT NULL
) distributed by (c_comment);

set gp_enable_fast_sri=off;

INSERT INTO customer_off VALUES (500, 'Customer#000000500', 'fy7qx5fHLhcbFL93duj9', 4, '14-194-736-4233', 3300.82, 'AUTOMOBILE', 's boost furiously. slyly special deposits sleep quickly above the furiously i');
INSERT INTO customer_off VALUES (502, 'Customer#000000502', 'nouAF6kednGsWEhQYyVpSnnPt', 11, '21-405-590-9919', 1378.67, 'HOUSEHOLD ', 'even asymptotes haggle. final, unusual theodolites haggle. carefully bo');
INSERT INTO customer_off VALUES (504, 'Customer#000000504', '2GuRx4pOLEQWU7fJOa, DYiK8IuMsXRLO5D 0', 10, '20-916-264-7594', 0.51, 'FURNITURE ', 'slyly final theodolites are across the carefully ');
INSERT INTO customer_off VALUES (506, 'Customer#000000506', 'dT kFaJww1B', 13, '23-895-781-8227', 1179.85, 'HOUSEHOLD ', ' idle instructions impress blithely along the carefully unusual notornis. furiously even packages');
INSERT INTO customer_off VALUES (508, 'Customer#000000508', 'q9Vq9 nTrUvx', 18, '28-344-250-3166', 1685.90, 'BUILDING  ', 'uses dazzle since the carefully regular accounts. patterns around the furiously even accounts wake blithely abov');
INSERT INTO customer_off VALUES (510, 'Customer#000000510', 'r6f34uxtNID YBuAXpO94BKyqjkM0qmT5n0Rmd9L', 5, '15-846-260-5139', 1572.48, 'HOUSEHOLD ', 'symptotes. furiously careful re');
INSERT INTO customer_off VALUES (513, 'Customer#000000513', 'sbWV6FIPas6C0puqgnKUI', 1, '11-861-303-6887', 955.37, 'HOUSEHOLD ', 'press along the quickly regular instructions. regular requests against the carefully ironic s');
INSERT INTO customer_off VALUES (515, 'Customer#000000515', 'oXxHtgXP5pXYTh', 15, '25-204-592-4731', 3225.07, 'BUILDING  ', 'ackages cajole furiously special, ironic deposits. carefully even Tiresias according to ');
INSERT INTO customer_off VALUES (517, 'Customer#000000517', 'mSo5eI8F4E6Kgl63nWtU84vfyQjOBg4y', 10, '20-475-741-4234', 3959.71, 'FURNITURE ', 'al, ironic foxes. packages wake according to the pending');
INSERT INTO customer_off VALUES (519, 'Customer#000000519', 'Z6ke6Y9J2pYuPBp7jE', 5, '15-452-860-5592', 9074.45, 'BUILDING  ', 'es. fluffily regular accounts should have to sleep quickly against the carefully ironic foxes. furiously daring');
INSERT INTO customer_off VALUES (521, 'Customer#000000521', 'MUEAEA1ZuvRofNY453Ckr4Apqk1GlOe', 2, '12-539-480-8897', 5830.69, 'MACHINERY ', 'ackages. stealthily even attainments sleep carefull');
INSERT INTO customer_off VALUES (523, 'Customer#000000523', 'sHeOSgsSnJi6pwYSr0v5ugiGhgnx7ZB', 10, '20-638-320-5977', -275.73, 'BUILDING  ', ' fluffily deposits. slyly regular instructions sleep e');
INSERT INTO customer_off VALUES (525, 'Customer#000000525', 'w0pOG5FhH45aYg7mKtHQhAWQKe', 19, '29-365-641-8287', 3931.68, 'AUTOMOBILE', ' blithely bold accounts about the quietl');
INSERT INTO customer_off VALUES (527, 'Customer#000000527', 'giJAUjnTtxX,HXIy0adwwvg,uu5Y3RVP', 13, '23-139-567-9286', 4429.81, 'HOUSEHOLD ', 'ending, ironic instructions. blithely regular deposits about the deposits wake pinto beans. closely silent ');
INSERT INTO customer_off VALUES (529, 'Customer#000000529', 'oGKgweC odpyORKPJ9oxTqzzdlYyFOwXm2F97C', 15, '25-383-240-7326', 9647.58, 'FURNITURE ', ' deposits after the fluffily special foxes integrate carefully blithely dogged dolphins. enticingly bold d');
INSERT INTO customer_off VALUES (531, 'Customer#000000531', 'ceI1iHfAaZ4DVVcm6GU370dAuIEmUW1wxG', 19, '29-151-567-1296', 5342.82, 'HOUSEHOLD ', 'e the brave, pending accounts. pending pinto beans above the ');
INSERT INTO customer_off VALUES (533, 'Customer#000000533', 'mSt8Gj4JqXXeDScn2CB PIrlnhvqxY,w6Ohku', 15, '25-525-957-4486', 5432.77, 'HOUSEHOLD ', 'even dolphins boost furiously among the theodo');
INSERT INTO customer_off VALUES (535, 'Customer#000000535', ',2Y kklprPasEp6DcthUibs', 2, '12-787-866-1808', 2912.80, 'BUILDING  ', 'even dinos breach. fluffily ironic');
INSERT INTO customer_off VALUES (537, 'Customer#000000537', 'wyXvxD,4jc', 10, '20-337-488-6765', 2614.79, 'FURNITURE ', 'e carefully blithely pending platelets. furiously final packages dazzle. ironic foxes wake f');
INSERT INTO customer_off VALUES (539, 'Customer#000000539', 'FoGcDu9llpFiB LELF3rdjaiw RQe1S', 6, '16-166-785-8571', 4390.33, 'HOUSEHOLD ', 'ent instructions. pending patter');
INSERT INTO customer_off VALUES (541, 'Customer#000000541', ',Cris88wkHw4Q0XlCLLYVOAJfkxw', 0, '10-362-308-9442', 1295.54, 'FURNITURE ', 'according to the final platelets. final, busy requests wake blithely across th');
INSERT INTO customer_off VALUES (543, 'Customer#000000543', 'JvbSKX7RG3xuqiKQ93C', 17, '27-972-408-3265', 6089.13, 'AUTOMOBILE', 'l, even theodolites. carefully bold accounts sleep about the sly');
INSERT INTO customer_off VALUES (545, 'Customer#000000545', 'AsYw6k,nDUQcMOpEws', 10, '20-849-123-8918', 7505.33, 'AUTOMOBILE', ' carefully final deposits. slyly unusual pinto beans may wake bold requests. unusual courts alongside ');
INSERT INTO customer_off VALUES (547, 'Customer#000000547', '4h SK3dVkE1tQ0NCh', 22, '32-696-724-2981', 6058.08, 'BUILDING  ', 'y express deposits. slyly ironic deposits nod slyly slyly ironic instructions. carefully quick idea');
INSERT INTO customer_off VALUES (549, 'Customer#000000549', 'v5uqfeHLiL1IELejUDnagWqP5pKWa9LtoemziGV', 24, '34-825-998-8579', 91.53, 'BUILDING  ', 'n asymptotes grow blithely. blithely fluffy deposits boost furiously. busily fu');
INSERT INTO customer_off VALUES (551, 'Customer#000000551', 'holp1DkjYzznatSwjG', 15, '25-209-544-4006', -334.89, 'MACHINERY ', 'y special ideas. slyly ironic foxes wake. regular packages alongside of the deposit');
INSERT INTO customer_off VALUES (553, 'Customer#000000553', '8tTlavJ sT', 4, '14-454-146-3094', 4804.57, 'BUILDING  ', 'ully regular requests are blithely about the express, bold platelets. slyly permanent deposits across the');
INSERT INTO customer_off VALUES (555, 'Customer#000000555', 'chm8jY6TfQ8CEnsvpuL6azNZzkqGcZcO8', 15, '25-548-367-9974', 5486.52, 'BUILDING  ', 'lites are blithely ironic ideas. blithely special pinto beans dazzl');
INSERT INTO customer_off VALUES (557, 'Customer#000000557', 'Nt6FUuDR7v', 15, '25-390-153-6699', 9559.04, 'BUILDING  ', 'furiously pending dolphins use. carefully unusual ideas must have to are carefully. express instructions a');
INSERT INTO customer_off VALUES (559, 'Customer#000000559', 'A3ACFoVbP,gPe xknVJMWC,wmRxb Nmg fWFS,UP', 7, '17-395-429-6655', 5872.94, 'AUTOMOBILE', 'al accounts cajole carefully across the accounts. furiously pending pinto beans across the ');
INSERT INTO customer_off VALUES (561, 'Customer#000000561', 'Z1kPCTbeTqGfdly2Ab9KEdE,jIKW', 18, '28-286-185-3047', 2323.45, 'FURNITURE ', 'across the furiously ironic theodolites. final requests cajole. slowly unusual foxes haggle carefully');
INSERT INTO customer_off VALUES (563, 'Customer#000000563', '2RSC1g7cVd,j23HusdkhdCGmiiE', 12, '22-544-152-1215', 3231.71, 'FURNITURE ', ' pinto beans believe fluffily. excuses wake blithely silent requests. b');
INSERT INTO customer_off VALUES (565, 'Customer#000000565', 'HCBXAou,1eP6Z3IynHFI7XmEBgu27Sx', 4, '14-798-211-2891', 2688.88, 'FURNITURE ', 'e. carefully bold deposits sleep regu');
INSERT INTO customer_off VALUES (567, 'Customer#000000567', 'KNE6mpW69IgTjVN', 21, '31-389-883-3371', 8475.17, 'BUILDING  ', 'blithe, even ideas. fluffily special requests wake. c');
INSERT INTO customer_off VALUES (569, 'Customer#000000569', 'Kk20Q5HiysjcPpMlL6pNUZXXuE', 2, '12-648-567-6776', -795.23, 'MACHINERY ', 'sh. blithely special excuses sleep. blithely ironic accounts slee');
INSERT INTO customer_off VALUES (571, 'Customer#000000571', 'hCrDDrMzGhsa6,5K4rGXQ', 2, '12-115-414-4819', 8993.23, 'HOUSEHOLD ', 'le fluffily. ironic, pending accounts poach quickly iron');
INSERT INTO customer_off VALUES (573, 'Customer#000000573', 'BEluH7it7jUcWqb tNLbMIKjU9hrnL7K', 4, '14-354-826-9743', 2333.96, 'HOUSEHOLD ', 'as. furiously even packages sleep quickly final excu');
INSERT INTO customer_off VALUES (575, 'Customer#000000575', '4K6h0pYH,bg2FS5cYL,qqejhvp7EfTlBjRjeVPkq', 1, '11-980-134-7627', 3652.29, 'BUILDING  ', ' final requests cajole after the ironic, bold instructio');
INSERT INTO customer_off VALUES (577, 'Customer#000000577', 'a73SSq2cip7C8nSzdmmscpZyLCZ7KL', 14, '24-662-826-1317', 7059.15, 'FURNITURE ', 'int furiously. slyly express pin');
INSERT INTO customer_off VALUES (579, 'Customer#000000579', '9ST2x,snyY3s', 0, '10-374-175-6181', 1924.96, 'MACHINERY ', 'ndencies detect slyly fluffil');
INSERT INTO customer_off VALUES (581, 'Customer#000000581', 's9SoN9XeVuCri', 24, '34-415-978-2518', 3242.10, 'MACHINERY ', 'ns. quickly regular pinto beans must sleep fluffily ');
INSERT INTO customer_off VALUES (583, 'Customer#000000583', 'V3i6Gu9,LZtvdnNppXnI2eKQFx0b36WvL,F ', 13, '23-234-625-4041', 3686.07, 'HOUSEHOLD ', ' haggle. regular, regular accounts hinder carefully i');
INSERT INTO customer_off VALUES (585, 'Customer#000000585', 'OAnZOqr6A,,77WC001ck8BAqvJTW6,dRGoRdX', 16, '26-397-693-4170', 7820.26, 'MACHINERY ', 'ickly ironic requests sleep regularly pending requ');
INSERT INTO customer_off VALUES (587, 'Customer#000000587', 'J2UwoJEQzAOTtuBrxGVag9iWSUPTp', 6, '16-585-233-5906', 7077.79, 'AUTOMOBILE', 've the final asymptotes. carefully final deposits wake fu');
INSERT INTO customer_off VALUES (589, 'Customer#000000589', 'TvdYNogIzDfr 1UyJE4b9RTENPmffmIoH', 19, '29-479-316-3576', 1647.05, 'FURNITURE ', 's; blithely ironic theodolites sleep-- accounts haggle around the furiously silent ideas. silent, final packages in');
INSERT INTO customer_off VALUES (591, 'Customer#000000591', 'wGE7AnEtiX7cmCkYA', 20, '30-584-309-7885', 6344.66, 'MACHINERY ', ' regular requests after the deposits cajole blithely ironic pinto beans. platelets about the regular, sp');
INSERT INTO customer_off VALUES (593, 'Customer#000000593', 'SYyEL2nytJXBbFemMseCiivA32USVEDbvGzZS', 9, '19-621-217-1535', 233.51, 'AUTOMOBILE', 've the regular, ironic deposits. requests along the special, regular theodolites lose furi');
INSERT INTO customer_off VALUES (595, 'Customer#000000595', '7Q17BacxM,liY2AwhnHGR0Pjf1180sMz1U', 19, '29-554-215-7805', 4177.17, 'HOUSEHOLD ', 'gular accounts x-ray carefully against the slyl');
INSERT INTO customer_off VALUES (597, 'Customer#000000597', 'Dbv,XVGzl4X', 15, '25-687-952-9485', 2443.52, 'AUTOMOBILE', 'es across the slyly brave packages maintain quickly quickly dogged excuses');
INSERT INTO customer_off VALUES (599, 'Customer#000000599', 'fIvpza0tlXAVjOAPkWN5,DiU0DO4e5NkfgOlXpDI', 4, '14-916-825-6916', 6004.52, 'HOUSEHOLD ', 'thely even requests wake carefully regular theodolites. instructions haggle alongside of the f');

set gp_enable_fast_sri=on;

INSERT INTO customer_on VALUES (500, 'Customer#000000500', 'fy7qx5fHLhcbFL93duj9', 4, '14-194-736-4233', 3300.82, 'AUTOMOBILE', 's boost furiously. slyly special deposits sleep quickly above the furiously i');
INSERT INTO customer_on VALUES (502, 'Customer#000000502', 'nouAF6kednGsWEhQYyVpSnnPt', 11, '21-405-590-9919', 1378.67, 'HOUSEHOLD ', 'even asymptotes haggle. final, unusual theodolites haggle. carefully bo');
INSERT INTO customer_on VALUES (504, 'Customer#000000504', '2GuRx4pOLEQWU7fJOa, DYiK8IuMsXRLO5D 0', 10, '20-916-264-7594', 0.51, 'FURNITURE ', 'slyly final theodolites are across the carefully ');
INSERT INTO customer_on VALUES (506, 'Customer#000000506', 'dT kFaJww1B', 13, '23-895-781-8227', 1179.85, 'HOUSEHOLD ', ' idle instructions impress blithely along the carefully unusual notornis. furiously even packages');
INSERT INTO customer_on VALUES (508, 'Customer#000000508', 'q9Vq9 nTrUvx', 18, '28-344-250-3166', 1685.90, 'BUILDING  ', 'uses dazzle since the carefully regular accounts. patterns around the furiously even accounts wake blithely abov');
INSERT INTO customer_on VALUES (510, 'Customer#000000510', 'r6f34uxtNID YBuAXpO94BKyqjkM0qmT5n0Rmd9L', 5, '15-846-260-5139', 1572.48, 'HOUSEHOLD ', 'symptotes. furiously careful re');
INSERT INTO customer_on VALUES (513, 'Customer#000000513', 'sbWV6FIPas6C0puqgnKUI', 1, '11-861-303-6887', 955.37, 'HOUSEHOLD ', 'press along the quickly regular instructions. regular requests against the carefully ironic s');
INSERT INTO customer_on VALUES (515, 'Customer#000000515', 'oXxHtgXP5pXYTh', 15, '25-204-592-4731', 3225.07, 'BUILDING  ', 'ackages cajole furiously special, ironic deposits. carefully even Tiresias according to ');
INSERT INTO customer_on VALUES (517, 'Customer#000000517', 'mSo5eI8F4E6Kgl63nWtU84vfyQjOBg4y', 10, '20-475-741-4234', 3959.71, 'FURNITURE ', 'al, ironic foxes. packages wake according to the pending');
INSERT INTO customer_on VALUES (519, 'Customer#000000519', 'Z6ke6Y9J2pYuPBp7jE', 5, '15-452-860-5592', 9074.45, 'BUILDING  ', 'es. fluffily regular accounts should have to sleep quickly against the carefully ironic foxes. furiously daring');
INSERT INTO customer_on VALUES (521, 'Customer#000000521', 'MUEAEA1ZuvRofNY453Ckr4Apqk1GlOe', 2, '12-539-480-8897', 5830.69, 'MACHINERY ', 'ackages. stealthily even attainments sleep carefull');
INSERT INTO customer_on VALUES (523, 'Customer#000000523', 'sHeOSgsSnJi6pwYSr0v5ugiGhgnx7ZB', 10, '20-638-320-5977', -275.73, 'BUILDING  ', ' fluffily deposits. slyly regular instructions sleep e');
INSERT INTO customer_on VALUES (525, 'Customer#000000525', 'w0pOG5FhH45aYg7mKtHQhAWQKe', 19, '29-365-641-8287', 3931.68, 'AUTOMOBILE', ' blithely bold accounts about the quietl');
INSERT INTO customer_on VALUES (527, 'Customer#000000527', 'giJAUjnTtxX,HXIy0adwwvg,uu5Y3RVP', 13, '23-139-567-9286', 4429.81, 'HOUSEHOLD ', 'ending, ironic instructions. blithely regular deposits about the deposits wake pinto beans. closely silent ');
INSERT INTO customer_on VALUES (529, 'Customer#000000529', 'oGKgweC odpyORKPJ9oxTqzzdlYyFOwXm2F97C', 15, '25-383-240-7326', 9647.58, 'FURNITURE ', ' deposits after the fluffily special foxes integrate carefully blithely dogged dolphins. enticingly bold d');
INSERT INTO customer_on VALUES (531, 'Customer#000000531', 'ceI1iHfAaZ4DVVcm6GU370dAuIEmUW1wxG', 19, '29-151-567-1296', 5342.82, 'HOUSEHOLD ', 'e the brave, pending accounts. pending pinto beans above the ');
INSERT INTO customer_on VALUES (533, 'Customer#000000533', 'mSt8Gj4JqXXeDScn2CB PIrlnhvqxY,w6Ohku', 15, '25-525-957-4486', 5432.77, 'HOUSEHOLD ', 'even dolphins boost furiously among the theodo');
INSERT INTO customer_on VALUES (535, 'Customer#000000535', ',2Y kklprPasEp6DcthUibs', 2, '12-787-866-1808', 2912.80, 'BUILDING  ', 'even dinos breach. fluffily ironic');
INSERT INTO customer_on VALUES (537, 'Customer#000000537', 'wyXvxD,4jc', 10, '20-337-488-6765', 2614.79, 'FURNITURE ', 'e carefully blithely pending platelets. furiously final packages dazzle. ironic foxes wake f');
INSERT INTO customer_on VALUES (539, 'Customer#000000539', 'FoGcDu9llpFiB LELF3rdjaiw RQe1S', 6, '16-166-785-8571', 4390.33, 'HOUSEHOLD ', 'ent instructions. pending patter');
INSERT INTO customer_on VALUES (541, 'Customer#000000541', ',Cris88wkHw4Q0XlCLLYVOAJfkxw', 0, '10-362-308-9442', 1295.54, 'FURNITURE ', 'according to the final platelets. final, busy requests wake blithely across th');
INSERT INTO customer_on VALUES (543, 'Customer#000000543', 'JvbSKX7RG3xuqiKQ93C', 17, '27-972-408-3265', 6089.13, 'AUTOMOBILE', 'l, even theodolites. carefully bold accounts sleep about the sly');
INSERT INTO customer_on VALUES (545, 'Customer#000000545', 'AsYw6k,nDUQcMOpEws', 10, '20-849-123-8918', 7505.33, 'AUTOMOBILE', ' carefully final deposits. slyly unusual pinto beans may wake bold requests. unusual courts alongside ');
INSERT INTO customer_on VALUES (547, 'Customer#000000547', '4h SK3dVkE1tQ0NCh', 22, '32-696-724-2981', 6058.08, 'BUILDING  ', 'y express deposits. slyly ironic deposits nod slyly slyly ironic instructions. carefully quick idea');
INSERT INTO customer_on VALUES (549, 'Customer#000000549', 'v5uqfeHLiL1IELejUDnagWqP5pKWa9LtoemziGV', 24, '34-825-998-8579', 91.53, 'BUILDING  ', 'n asymptotes grow blithely. blithely fluffy deposits boost furiously. busily fu');
INSERT INTO customer_on VALUES (551, 'Customer#000000551', 'holp1DkjYzznatSwjG', 15, '25-209-544-4006', -334.89, 'MACHINERY ', 'y special ideas. slyly ironic foxes wake. regular packages alongside of the deposit');
INSERT INTO customer_on VALUES (553, 'Customer#000000553', '8tTlavJ sT', 4, '14-454-146-3094', 4804.57, 'BUILDING  ', 'ully regular requests are blithely about the express, bold platelets. slyly permanent deposits across the');
INSERT INTO customer_on VALUES (555, 'Customer#000000555', 'chm8jY6TfQ8CEnsvpuL6azNZzkqGcZcO8', 15, '25-548-367-9974', 5486.52, 'BUILDING  ', 'lites are blithely ironic ideas. blithely special pinto beans dazzl');
INSERT INTO customer_on VALUES (557, 'Customer#000000557', 'Nt6FUuDR7v', 15, '25-390-153-6699', 9559.04, 'BUILDING  ', 'furiously pending dolphins use. carefully unusual ideas must have to are carefully. express instructions a');
INSERT INTO customer_on VALUES (559, 'Customer#000000559', 'A3ACFoVbP,gPe xknVJMWC,wmRxb Nmg fWFS,UP', 7, '17-395-429-6655', 5872.94, 'AUTOMOBILE', 'al accounts cajole carefully across the accounts. furiously pending pinto beans across the ');
INSERT INTO customer_on VALUES (561, 'Customer#000000561', 'Z1kPCTbeTqGfdly2Ab9KEdE,jIKW', 18, '28-286-185-3047', 2323.45, 'FURNITURE ', 'across the furiously ironic theodolites. final requests cajole. slowly unusual foxes haggle carefully');
INSERT INTO customer_on VALUES (563, 'Customer#000000563', '2RSC1g7cVd,j23HusdkhdCGmiiE', 12, '22-544-152-1215', 3231.71, 'FURNITURE ', ' pinto beans believe fluffily. excuses wake blithely silent requests. b');
INSERT INTO customer_on VALUES (565, 'Customer#000000565', 'HCBXAou,1eP6Z3IynHFI7XmEBgu27Sx', 4, '14-798-211-2891', 2688.88, 'FURNITURE ', 'e. carefully bold deposits sleep regu');
INSERT INTO customer_on VALUES (567, 'Customer#000000567', 'KNE6mpW69IgTjVN', 21, '31-389-883-3371', 8475.17, 'BUILDING  ', 'blithe, even ideas. fluffily special requests wake. c');
INSERT INTO customer_on VALUES (569, 'Customer#000000569', 'Kk20Q5HiysjcPpMlL6pNUZXXuE', 2, '12-648-567-6776', -795.23, 'MACHINERY ', 'sh. blithely special excuses sleep. blithely ironic accounts slee');
INSERT INTO customer_on VALUES (571, 'Customer#000000571', 'hCrDDrMzGhsa6,5K4rGXQ', 2, '12-115-414-4819', 8993.23, 'HOUSEHOLD ', 'le fluffily. ironic, pending accounts poach quickly iron');
INSERT INTO customer_on VALUES (573, 'Customer#000000573', 'BEluH7it7jUcWqb tNLbMIKjU9hrnL7K', 4, '14-354-826-9743', 2333.96, 'HOUSEHOLD ', 'as. furiously even packages sleep quickly final excu');
INSERT INTO customer_on VALUES (575, 'Customer#000000575', '4K6h0pYH,bg2FS5cYL,qqejhvp7EfTlBjRjeVPkq', 1, '11-980-134-7627', 3652.29, 'BUILDING  ', ' final requests cajole after the ironic, bold instructio');
INSERT INTO customer_on VALUES (577, 'Customer#000000577', 'a73SSq2cip7C8nSzdmmscpZyLCZ7KL', 14, '24-662-826-1317', 7059.15, 'FURNITURE ', 'int furiously. slyly express pin');
INSERT INTO customer_on VALUES (579, 'Customer#000000579', '9ST2x,snyY3s', 0, '10-374-175-6181', 1924.96, 'MACHINERY ', 'ndencies detect slyly fluffil');
INSERT INTO customer_on VALUES (581, 'Customer#000000581', 's9SoN9XeVuCri', 24, '34-415-978-2518', 3242.10, 'MACHINERY ', 'ns. quickly regular pinto beans must sleep fluffily ');
INSERT INTO customer_on VALUES (583, 'Customer#000000583', 'V3i6Gu9,LZtvdnNppXnI2eKQFx0b36WvL,F ', 13, '23-234-625-4041', 3686.07, 'HOUSEHOLD ', ' haggle. regular, regular accounts hinder carefully i');
INSERT INTO customer_on VALUES (585, 'Customer#000000585', 'OAnZOqr6A,,77WC001ck8BAqvJTW6,dRGoRdX', 16, '26-397-693-4170', 7820.26, 'MACHINERY ', 'ickly ironic requests sleep regularly pending requ');
INSERT INTO customer_on VALUES (587, 'Customer#000000587', 'J2UwoJEQzAOTtuBrxGVag9iWSUPTp', 6, '16-585-233-5906', 7077.79, 'AUTOMOBILE', 've the final asymptotes. carefully final deposits wake fu');
INSERT INTO customer_on VALUES (589, 'Customer#000000589', 'TvdYNogIzDfr 1UyJE4b9RTENPmffmIoH', 19, '29-479-316-3576', 1647.05, 'FURNITURE ', 's; blithely ironic theodolites sleep-- accounts haggle around the furiously silent ideas. silent, final packages in');
INSERT INTO customer_on VALUES (591, 'Customer#000000591', 'wGE7AnEtiX7cmCkYA', 20, '30-584-309-7885', 6344.66, 'MACHINERY ', ' regular requests after the deposits cajole blithely ironic pinto beans. platelets about the regular, sp');
INSERT INTO customer_on VALUES (593, 'Customer#000000593', 'SYyEL2nytJXBbFemMseCiivA32USVEDbvGzZS', 9, '19-621-217-1535', 233.51, 'AUTOMOBILE', 've the regular, ironic deposits. requests along the special, regular theodolites lose furi');
INSERT INTO customer_on VALUES (595, 'Customer#000000595', '7Q17BacxM,liY2AwhnHGR0Pjf1180sMz1U', 19, '29-554-215-7805', 4177.17, 'HOUSEHOLD ', 'gular accounts x-ray carefully against the slyl');
INSERT INTO customer_on VALUES (597, 'Customer#000000597', 'Dbv,XVGzl4X', 15, '25-687-952-9485', 2443.52, 'AUTOMOBILE', 'es across the slyly brave packages maintain quickly quickly dogged excuses');
INSERT INTO customer_on VALUES (599, 'Customer#000000599', 'fIvpza0tlXAVjOAPkWN5,DiU0DO4e5NkfgOlXpDI', 4, '14-916-825-6916', 6004.52, 'HOUSEHOLD ', 'thely even requests wake carefully regular theodolites. instructions haggle alongside of the f');

SELECT * from customer_off a, customer_off b where a.gp_segment_id <> b.gp_segment_id and a.c_custkey = b.c_custkey;

-- Test partitioned tables which have child with different distribution policies

set gp_enable_fast_sri to on;
-- single level case
create table pt (i int, j int, k int) distributed by (i) partition by range(k)
(start(1) end(10) every(2));
alter table pt_1_prt_1 set distributed by (j);
alter table pt_1_prt_2 set distributed randomly;
alter table pt_1_prt_2 set distributed by (k);
insert into pt values(1, 1, 1);
insert into pt values(2, 2, 2);
insert into pt values(3, 3, 3);
insert into pt values(4, 4, 4);
insert into pt values(5, 5, 5);
insert into pt values(6, 6, 6);
insert into pt values(7, 7, 7);

select * from pt;

create table pt_1 (i int, j int, k int) distributed by (j);
create table pt_2 (i int, j int, k int) distributed randomly;
create table pt_3 (i int, j int, k int) distributed by (k);
create table pt_4 (i int, j int, k int) distributed by (i);

insert into pt_1 values(1, 1, 1);
insert into pt_1 values(2, 2, 2);
insert into pt_2 values(3, 3, 3);
insert into pt_2 values(4, 4, 4);
insert into pt_3 values(5, 5, 5);
insert into pt_3 values(6, 6, 6);
insert into pt_4 values(7, 7, 7);

select gp_segment_id, * from pt_1_prt_1 except 
select gp_segment_id, * from pt_1;
select gp_segment_id, * from pt_1_prt_3 except 
select gp_segment_id, * from pt_3;
select gp_segment_id, * from pt_1_prt_4 except 
select gp_segment_id, * from pt_4;
drop table pt, pt_1, pt_2, pt_3, pt_4;

-- sub partitioned case
create table pt(i int, j int, k int, l char(2)) distributed by (i)
partition by range(k) subpartition by list(l) subpartition template(values('A'),
values('B')) (start(30) end(50) every(10));
alter table only pt set distributed randomly;

insert into pt values(35, 35, 35, 'A');
insert into pt values(36, 35, 35, 'A');
insert into pt values(37, 35, 35, 'A');
insert into pt values(38, 35, 35, 'A');

create table pt_1 (like pt_1_prt_1) distributed by (i);
insert into pt_1 values(35, 35, 35, 'A');
insert into pt_1 values(36, 35, 35, 'A');
insert into pt_1 values(37, 35, 35, 'A');
insert into pt_1 values(38, 35, 35, 'A');

select gp_segment_id, * from pt except
select gp_segment_id, * from pt_1;

alter table only pt_1_prt_2 set distributed by (j);
insert into pt values(45, 45, 45, 'B');
insert into pt values(45, 46, 46, 'B');
insert into pt values(45, 47, 47, 'B');
create table pt_2 (like pt_1_prt_2_2_prt_2) distributed by (i);
insert into pt_2 values(45, 45, 45, 'B');
insert into pt_2 values(45, 46, 46, 'B');
insert into pt_2 values(45, 47, 47, 'B');

select gp_segment_id, * from pt  where k > 40 and l = 'B' except
select gp_segment_id, * from pt_2;

alter table pt_1_prt_2_2_prt_1 set distributed by (j);
insert into pt values(45, 45, 45, 'A');
insert into pt values(45, 46, 46, 'A');
insert into pt values(45, 47, 47, 'A');
create table pt_3 (like pt_1_prt_2_2_prt_1) distributed by (i);
insert into pt_3 values(45, 45, 45, 'A');
insert into pt_3 values(45, 46, 46, 'A');
insert into pt_3 values(45, 47, 47, 'A');

select gp_segment_id, * from pt  where k > 40 and l = 'A' except
select gp_segment_id, * from pt_3;

-- make sure we're not seeing things
select * from pt order by 1, 2, 3, 4;
drop table pt, pt_1, pt_2, pt_3;

-- Make sure all array types work
create table mpp5746 (c _int4) distributed by (c);
create table mpp5746_2 (c int4[]) distributed by (c);
insert into mpp5746 select array[i] from generate_series(1, 100) i;
insert into mpp5746_2 select array[i] from generate_series(1, 100) i;
select gp_segment_id, * from mpp5746 except
select gp_segment_id, * from mpp5746_2;
drop table mpp5746_2;

create table mpp5746_2 as select * from mpp5746 distributed by (c);
select gp_segment_id, * from mpp5746 except
select gp_segment_id, * from mpp5746_2;
drop table mpp5746, mpp5746_2;

--
-- Test that NULLs are distributed correctly, by a CTAS involving an outer join
--
create temporary table even (i int4, j int4) distributed by (i);
insert into even select g*2, g*2 from generate_series(1, 10) g;
create temporary table odd (i int4, j int4) distributed by (i);
insert into odd select g*2+1, g*2+1 from generate_series(1, 10) g;

create temporary table ctas_x as
  select even.j, even.i as a, odd.i as b from even full outer join odd on (even.i = odd.i)
distributed by (a);

-- Check that all the rows with NULL distribution key are stored on the same segment.
select count(distinct gp_segment_id) from ctas_x where a is null;
select a from ctas_x group by a;

-- The same, but let the planner deduce the distribution key by itself. The
-- codepaths to deduce it, and to check if the explicitly given distribution
-- needs a Redistribute, are different. It should not choose 'a', even though
-- the result is distributed on 'a', because there are NULLs on all segments.
create temporary table ctas_y as
  select even.j, even.i as a, odd.i as b
  from even full outer join odd on (even.i = odd.i);

select a from ctas_y group by a;

-- Same for INSERT.
create temporary table insert_z (j int4, a int4, b int4) distributed by (a);
insert into insert_z
  select even.j, even.i as a, odd.i as b
  from even full outer join odd on (even.i = odd.i);

select count(distinct gp_segment_id) from insert_z where a is null;
select a from insert_z group by a;

-- This doesn't need a Redistribute Motion.
explain select even.i from even left outer join odd on (even.i = odd.i) group by (even.i);

-- But this does.
explain select even.i from even right outer join odd on (even.i = odd.i) group by (even.i);

-- Check that we can track the distribution through multiple FULL OUTER JOINs.
-- This query should not need Redistribute Motion.
create temporary table a as select generate_series(1, 5) as i distributed by (i);
create temporary table b as select generate_series(2, 6) as i distributed by (i);
create temporary table c as select generate_series(3, 7) as i distributed by (i);
explain select * from a full join b on (a.i=b.i) full join c on (b.i=c.i);
select * from a full join b on (a.i=b.i) full join c on (b.i=c.i);
