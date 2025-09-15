import requests
import csv
import json
from datetime import datetime

def truncate_string(s, length):
    """Truncate and pad string to exact length"""
    return str(s)[:length]

def fetch_relational_data():
    """Fetch related data from dummyjson.com"""

    # 1. Fetch Users (customers)
    print("Fetching users...")
    users_response = requests.get('https://dummyjson.com/users?limit=100')
    users = users_response.json()['users']

    with open('users.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['user_id', 'username', 'email', 'age', 'city'])

        for user in users:
            writer.writerow([
                user['id'],  # uint32
                truncate_string(user['username'], 16),  # char16
                truncate_string(user['email'], 32),  # char32
                user['age'],  # uint32
                truncate_string(user['address']['city'], 16)  # char16
            ])

    print(f"  Fetched {len(users)} users")

    # 2. Fetch Products
    print("Fetching products...")
    products_response = requests.get('https://dummyjson.com/products?limit=100')
    products = products_response.json()['products']

    with open('products.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['product_id', 'title', 'category', 'price', 'stock', 'brand'])

        for product in products:
            writer.writerow([
                product['id'],  # uint32
                truncate_string(product['title'], 32),  # char32
                truncate_string(product['category'], 16),  # char16
                int(product['price']),  # uint32 (converting from float)
                product['stock'],  # uint32
                truncate_string(product.get('brand', 'Generic'), 16)  # char16 with default
            ])

    print(f"  Fetched {len(products)} products")

    # 3. Fetch Carts (Orders with line items - ONE TO MANY)
    print("Fetching carts/orders...")
    carts_response = requests.get('https://dummyjson.com/carts?limit=20')
    carts = carts_response.json()['carts']

    order_id_counter = 1000
    order_items = []
    orders = []

    for cart in carts:
        # Create order header
        orders.append({
            'order_id': order_id_counter,
            'user_id': cart['userId'],
            'total': int(cart['total']),
            'total_quantity': cart['totalQuantity'],
            'discount': int(cart['discountedTotal'])
        })

        # Create order items (one-to-many relationship)
        for item in cart['products']:
            order_items.append({
                'item_id': len(order_items) + 1,
                'order_id': order_id_counter,  # FK to orders
                'product_id': item['id'],  # FK to products
                'quantity': item['quantity'],
                'price': int(item['price']),
                'total': int(item['total'])
            })

        order_id_counter += 1

    # Write orders
    with open('orders.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['order_id', 'user_id', 'total', 'total_quantity', 'discount'])
        for order in orders:
            writer.writerow([order['order_id'], order['user_id'], order['total'],
                           order['total_quantity'], order['discount']])

    # Write order_items (junction table)
    with open('order_items.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['item_id', 'order_id', 'product_id', 'quantity', 'price', 'total'])
        for item in order_items:
            writer.writerow([item['item_id'], item['order_id'], item['product_id'],
                           item['quantity'], item['price'], item['total']])

    print(f"  Fetched {len(orders)} orders with {len(order_items)} line items")

    # 4. Fetch Posts and Comments (ONE TO MANY)
    print("Fetching posts and comments...")
    posts_response = requests.get('https://dummyjson.com/posts?limit=50')
    posts = posts_response.json()['posts']

    comments_response = requests.get('https://dummyjson.com/comments?limit=200')
    comments = comments_response.json()['comments']

    # Write posts
    with open('posts.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['post_id', 'user_id', 'title', 'views', 'reactions'])

        for post in posts:
            writer.writerow([
                post['id'],  # uint32
                post['userId'],  # uint32 FK to users
                truncate_string(post['title'], 32),  # char32
                post.get('views', 0),  # uint32 with default
                post.get('reactions', 0)  # uint32 with default
            ])

    # Write comments
    with open('comments.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['comment_id', 'post_id', 'user_id', 'body', 'likes'])

        for comment in comments:
            writer.writerow([
                comment['id'],  # uint32
                comment['postId'],  # uint32 FK to posts
                comment['user']['id'],  # uint32 FK to users
                truncate_string(comment['body'], 32),  # char32
                comment.get('likes', 0)  # uint32 with default
            ])

    print(f"  Fetched {len(posts)} posts and {len(comments)} comments")

    # 5. Create Tags and Post_Tags (MANY TO MANY)
    print("Creating tags and relationships...")

    # Extract unique tags from posts
    all_tags = set()
    for post in posts:
        all_tags.update(post.get('tags', []))

    tags_list = list(all_tags)

    # Write tags
    with open('tags.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['tag_id', 'tag_name'])

        for i, tag in enumerate(tags_list, 1):
            writer.writerow([
                i,  # uint32
                truncate_string(tag, 16)  # char16
            ])

    # Write post_tags (junction table for many-to-many)
    post_tags = []
    for post in posts:
        for tag in post.get('tags', []):
            tag_id = tags_list.index(tag) + 1
            post_tags.append({
                'post_id': post['id'],
                'tag_id': tag_id
            })

    with open('post_tags.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['post_id', 'tag_id'])
        for pt in post_tags:
            writer.writerow([pt['post_id'], pt['tag_id']])

    print(f"  Created {len(tags_list)} tags with {len(post_tags)} relationships")

    # 6. Create User_Followers (MANY TO MANY self-referential)
    print("Creating follower relationships...")

    # Generate some follower relationships
    followers = []
    for i in range(1, min(31, len(users) + 1)):  # First 30 users
        # Each user follows 2-5 other users
        num_following = min(5, len(users) - 1)
        for j in range(num_following):
            followed_id = ((i + j + 1) % len(users)) + 1
            if followed_id != i:  # Don't follow yourself
                followers.append({
                    'follower_id': i,
                    'followed_id': followed_id
                })

    with open('user_followers.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['follower_id', 'followed_id'])
        for follow in followers:
            writer.writerow([follow['follower_id'], follow['followed_id']])

    print(f"  Created {len(followers)} follower relationships")

    print("\n✅ Data fetched successfully!")
    print("\n📊 Summary:")
    print(f"  - {len(users)} users")
    print(f"  - {len(products)} products")
    print(f"  - {len(orders)} orders with {len(order_items)} line items")
    print(f"  - {len(posts)} posts with {len(comments)} comments")
    print(f"  - {len(tags_list)} unique tags")
    print(f"  - {len(post_tags)} post-tag relationships")
    print(f"  - {len(followers)} follower relationships")

    print("\n🔗 Relationships:")
    print("  ONE-TO-MANY:")
    print("    - orders -> users")
    print("    - order_items -> orders")
    print("    - order_items -> products")
    print("    - posts -> users")
    print("    - comments -> posts")
    print("    - comments -> users")
    print("  MANY-TO-MANY:")
    print("    - posts <-> tags (via post_tags)")
    print("    - users <-> users (via user_followers)")

if __name__ == "__main__":
    try:
        fetch_relational_data()
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
