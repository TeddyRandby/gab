def bottom_up_tree(item, depth):
    if depth != 0: 
        i = item + item
        left = bottom_up_tree(i - 1, depth - 1)
        right = bottom_up_tree(i, depth - 1)
        return [item, left, right]
    else:
        return [item]

def item_check(tree):
    if (len(tree) != 1):
        return tree[0] + item_check(tree[1]) - item_check(tree[2])
    else: 
        return tree[0]
  


def pow2(n):
    if (n == 0):
        return 1
    else:
        return pow2(n - 1) * 2

def run(N):
    mindepth = 4
    maxdepth = mindepth + 2

    if (maxdepth < N):
        maxdepth = N

    stretchdepth = maxdepth + 1
    tree = bottom_up_tree(0, stretchdepth)
    print(item_check(tree))

    longlivedtree = bottom_up_tree(0, maxdepth)

    depth = mindepth
    while (depth < maxdepth + 1):
        iters = pow2(maxdepth - depth + mindepth);
        check = 0;
        checks = 0;

        while (check < iters):
          tree1 = bottom_up_tree(1, depth)
          checks = checks + item_check(tree1)
          tree2 = bottom_up_tree(0 - 1, depth)
          checks = checks + item_check(tree2)
          check = check + 1

        print(checks);
        depth = depth + 2;

    print(item_check(longlivedtree));

run(13)
